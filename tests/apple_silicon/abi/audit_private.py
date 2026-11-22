#!/usr/bin/env python3
"""Audit a private generated-core build without printing private identifiers."""

import argparse
import hashlib
import hmac
import importlib.util
import json
import re
import shlex
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / "kit/source/game"
PATCHES = ROOT / "kit/patches"
DEFINITION = re.compile(
    r"(?m)^[ \t]*(?!static\b)(?:extern[ \t]+\"C\"[ \t]+)?"
    r"(?:[A-Za-z_]\w*[ \t]+|[A-Za-z_]\w*[ \t]*\*+[ \t]*){1,8}"
    r"([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t\r\n]*\{"
)
DEFERRED_APPLE_CONTRACT_SOURCES = frozenset({
    "src/bink_native.c",
    "src/bots/ma2_bot_c7.c",
    "src/bots/ma2_bot_ctrl.c",
    "src/bots/ma2_bot_nav.c",
    "src/bots/ma2_bot_player.c",
    "src/bots/ma2_bots_core.c",
    "src/ma2_cinematic.c",
    "src/ma2_tank_trace.c",
    "src/pc_gui_bridge.c",
    "src/pc_native_settings.cpp",
    "src/pc_prompts.cpp",
    "src/pc_settings.cpp",
    "src/scenario/ma2_scenario.c",
})


class AuditError(Exception):
    pass


def manifests(generated):
    stages = [
        json.loads((PATCHES / "manifest.json").read_text(encoding="utf-8")),
        json.loads((PATCHES / "fastcode/fastcode.json").read_text(encoding="utf-8")),
        json.loads((PATCHES / "fastcode/post/manifest.json").read_text(encoding="utf-8")),
    ]
    final = {}
    for stage_number, stage in enumerate(stages):
        for name, digest in stage["final"].items():
            if Path(name).name != name:
                raise AuditError("invalid manifest entry")
            final[name] = (digest, stage_number)
    normalized_lf = set(stages[1]["lf"])
    mismatches = 0
    for name, (expected, stage_number) in final.items():
        try:
            data = (generated / name).read_bytes()
        except OSError:
            mismatches += 1
            continue
        actual = hashlib.sha256(data).hexdigest()
        if (not hmac.compare_digest(actual, expected) and stage_number == 0 and
                name in normalized_lf and b"\r" not in data):
            actual = hashlib.sha256(data.replace(b"\n", b"\r\n")).hexdigest()
        mismatches += not hmac.compare_digest(actual, expected)
    return len(final), mismatches


def command_arguments(entry):
    if "arguments" in entry:
        return entry["arguments"]
    return shlex.split(entry["command"])


def option_value(arguments, option):
    indexes = [index for index, value in enumerate(arguments) if value == option]
    if len(indexes) != 1 or indexes[0] + 1 == len(arguments):
        raise AuditError("compiler option missing or ambiguous")
    return arguments[indexes[0] + 1]


def verify_overlay(overlay_path, header, expected, require_private=True):
    overlay_path = overlay_path.resolve(strict=True)
    if require_private and (overlay_path.is_relative_to(ROOT) or
                            stat.S_IMODE(overlay_path.parent.stat().st_mode) & 0o077 or
                            stat.S_IMODE(overlay_path.stat().st_mode) & 0o077):
        raise AuditError("ABI overlay is not private")
    overlay = json.loads(overlay_path.read_text(encoding="utf-8"))
    roots = overlay.get("roots", [])
    if (overlay.get("version") != 0 or overlay.get("use-external-names") is not False or
            len(roots) != 1 or roots[0].get("type") != "file"):
        raise AuditError("unexpected ABI overlay")
    if Path(roots[0]["name"]).resolve() != header.resolve():
        raise AuditError("ABI overlay does not target the generated header")
    adapted = Path(roots[0]["external-contents"]).resolve(strict=True)
    if require_private and (adapted.is_relative_to(ROOT) or
                            stat.S_IMODE(adapted.parent.stat().st_mode) & 0o077 or
                            stat.S_IMODE(adapted.stat().st_mode) & 0o077):
        raise AuditError("adapted ABI header is not private")
    if adapted.read_bytes() != expected:
        raise AuditError("adapted ABI header does not match the verified transformation")


def compiled_core(build, generated):
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    sources = {path.resolve() for path in generated.glob("*.c") if path.is_file()}
    if not sources:
        raise AuditError("no generated sources")
    compiled = set()
    objects = set()
    overlay_checks = 0
    core_checks = 0
    adapter_path = GAME / "src/apple/abi/prepare_overlay.py"
    spec = importlib.util.spec_from_file_location("ma2_private_abi_overlay", adapter_path)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    expected = adapter.adapt_data((generated / "recomp_types.h").read_bytes())
    checked_overlays = set()
    for entry in commands:
        args = command_arguments(entry)
        try:
            output = option_value(args, "-o")
        except AuditError:
            continue
        if "CMakeFiles/ma2_game_core.dir/" not in output:
            continue
        directory = Path(entry["directory"])
        obj = Path(output)
        obj = (directory / obj).resolve() if not obj.is_absolute() else obj.resolve()
        if not obj.is_file():
            raise AuditError("core object missing")
        objects.add(obj)
        overlay_path = Path(option_value(args, "-ivfsoverlay"))
        if not overlay_path.is_absolute():
            overlay_path = directory / overlay_path
        overlay_path = overlay_path.resolve()
        if overlay_path not in checked_overlays:
            verify_overlay(overlay_path, generated / "recomp_types.h", expected)
            checked_overlays.add(overlay_path)
        if Path(option_value(args, "-include")).resolve() != (
                GAME / "src/apple/recomp_compat.h").resolve():
            raise AuditError("core source lacks the compatibility header")
        core_checks += 1
        source = Path(entry["file"]).resolve()
        if source.parent == generated:
            if source in compiled:
                raise AuditError("generated source compiled more than once")
            compiled.add(source)
            overlay_checks += 1
    return sources, compiled, objects, overlay_checks, core_checks


def architectures(paths):
    failures = 0
    for path in paths:
        result = subprocess.run(["lipo", "-archs", str(path)], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, check=False)
        failures += result.returncode != 0 or "arm64" not in result.stdout.split()
    return failures


def symbols(paths, undefined):
    result = set()
    ordered = sorted(str(path) for path in paths)
    for offset in range(0, len(ordered), 24):
        command = ["nm", "-guj" if undefined else "-gUj", *ordered[offset:offset + 24]]
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, check=False)
        if proc.returncode:
            raise AuditError("symbol tool failed")
        result.update(line.strip() for line in proc.stdout.splitlines()
                      if line.startswith("_") and not line.endswith(":"))
    return result


def tracked_definition_sources():
    proc = subprocess.run(["git", "ls-files", "-z", "--", "src"], cwd=GAME,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    if proc.returncode:
        raise AuditError("tracked source inventory unavailable")
    sources = {}
    for item in proc.stdout.split(b"\0"):
        if not item:
            continue
        relative = item.decode("utf-8")
        path = GAME / relative
        if path.suffix not in {".c", ".cpp", ".h", ".hpp", ".m", ".mm"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
        for name in DEFINITION.findall(text):
            sources.setdefault(name, set()).add(relative)
    if not sources:
        raise AuditError("tracked source inventory is empty")
    return sources


def unresolved_tracked(undefined, defined, candidates):
    return {symbol for symbol in undefined - defined
            if symbol.startswith("_") and symbol[1:] in candidates}


def audit(generated, build):
    generated = generated.resolve(strict=True)
    build = build.resolve(strict=True)
    if generated.is_relative_to(ROOT) or build.is_relative_to(ROOT):
        raise AuditError("private inputs and build must remain outside the repository")
    manifest_count, manifest_mismatches = manifests(generated)
    sources, compiled, core_objects, overlay_count, core_checks = compiled_core(build, generated)
    objects = set(build.rglob("*.o"))
    archives = set(build.rglob("*.a"))
    if not objects or not archives or not core_objects:
        raise AuditError("incomplete private build")
    arch_failures = architectures(objects | archives)
    defined = symbols(core_objects | archives, False)
    undefined = symbols(core_objects | archives, True)
    remaining = undefined - defined
    definition_sources = tracked_definition_sources()
    available_sources = {
        source for source_set in definition_sources.values() for source in source_set
    }
    if not DEFERRED_APPLE_CONTRACT_SOURCES <= available_sources:
        raise AuditError("deferred Apple contract inventory is stale")
    tracked = unresolved_tracked(undefined, defined, set(definition_sources))
    unexpected = {
        symbol for symbol in tracked
        if not definition_sources[symbol[1:]] <= DEFERRED_APPLE_CONTRACT_SOURCES
    }
    deferred = tracked - unexpected
    contract_sources = {}
    for symbol in deferred:
        for source in definition_sources[symbol[1:]]:
            contract_sources[source] = contract_sources.get(source, 0) + 1
    print(f"Final manifest: {manifest_count} entries, {manifest_mismatches} mismatches")
    print(f"Generated source coverage: {len(sources)} discovered, {len(compiled)} compiled, "
          f"{len(sources - compiled)} missing, {len(compiled - sources)} unexpected")
    print(f"Verified ABI overlay: {overlay_count} generated and {core_checks} core compilations")
    print(f"Architecture: {len(objects)} objects, {len(archives)} archives, "
          f"{arch_failures} without an arm64 slice")
    print(f"External symbol sets: {len(defined)} definitions, {len(undefined)} references, "
          f"{len(remaining)} unresolved after aggregation")
    print(f"Explicitly deferred Apple contracts: {len(deferred)}")
    for source, count in sorted(contract_sources.items()):
        print(f"  {source}: {count}")
    print(f"Unexpected omitted tracked contracts: {len(unexpected)}")
    if (manifest_mismatches or sources != compiled or overlay_count != len(sources) or
            arch_failures or unexpected):
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-dir", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        return audit(args.generated_dir, args.build_dir)
    except AuditError as error:
        print(f"Private ABI audit failed: {error}", file=sys.stderr)
        return 2
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"Private ABI audit failed: {type(error).__name__}",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
