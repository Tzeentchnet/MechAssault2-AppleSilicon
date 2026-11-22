#!/usr/bin/env python3
"""Synthetic, native Apple Silicon ABI checks; no game data is required."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import unittest
from unittest import mock
import uuid


ROOT = Path(__file__).resolve().parents[2]
ABI = ROOT / "tests/apple_silicon/abi"
APPLE = ROOT / "kit/source/game/src/apple"
RUNTIME = ROOT / "kit/source/runtime"
TEMPLATE = ROOT / "kit/toolkit/templates/runtime/recomp_types.h"
TARGETS = (
    "abi_numeric", "abi_dispatch", "abi_threads", "abi_unicode_atomics",
    "abi_trace_concurrency", "abi_cxx_linkage", "abi_audio_lifetime",
)
ASAN_TARGETS = TARGETS
TSAN_TARGETS = ("abi_trace_concurrency", "abi_unicode_atomics", "abi_threads")


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() in {"arm64", "aarch64"},
                     "requires native macOS arm64")
class Arm64ABITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.chdir(ROOT)
        cls.work = ROOT / "_work/apple-silicon/abi-tests" / uuid.uuid4().hex
        cls.work.mkdir(parents=True, mode=0o700)
        scratch = cls.work / "compiler"
        scratch.mkdir(mode=0o700)
        cls.env = dict(os.environ, TMPDIR=str(scratch), RECOMP_HEADLESS="1",
                       SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=0",
                       TSAN_OPTIONS="halt_on_error=1")
        try:
            cls.clang = subprocess.check_output(
                ["xcrun", "--find", "clang"], text=True).strip()
            cls.sdk = subprocess.check_output(
                ["xcrun", "--show-sdk-path"], text=True).strip()
            cls.adapter = load_module("ma2_abi_overlay", APPLE / "abi/prepare_overlay.py")
            cls.audit = load_module("ma2_abi_audit", ABI / "audit_private.py")
            cls.overlay = cls.adapter.prepare(TEMPLATE, cls.work / "overlay", template=True)
            sdl = os.environ.get("SDL2_DIR")
            if not sdl:
                prefix = subprocess.check_output(
                    ["brew", "--prefix", "sdl2-compat"], text=True).strip()
                sdl = str(Path(prefix) / "lib/cmake/SDL2")
            openssl = os.environ.get("OPENSSL_ROOT_DIR")
            if not openssl:
                openssl = subprocess.check_output(
                    ["brew", "--prefix", "openssl@3"], text=True).strip()
            sdl3 = os.environ.get("SDL3_LIBRARY_DIR")
            if not sdl3:
                try:
                    prefix = subprocess.check_output(
                        ["brew", "--prefix", "sdl3"], text=True,
                        stderr=subprocess.DEVNULL).strip()
                    sdl3 = str(Path(prefix) / "lib")
                except (FileNotFoundError, subprocess.CalledProcessError):
                    sdl3 = None
            if sdl3 and (Path(sdl3) / "libSDL3.dylib").is_file():
                fallback = cls.env.get("DYLD_FALLBACK_LIBRARY_PATH")
                cls.env["DYLD_FALLBACK_LIBRARY_PATH"] = (
                    fallback + os.pathsep + sdl3 if fallback else sdl3)
            cls.builds = {}
            for name, mode, sanitizer in (
                    ("release", "Release", ""),
                    ("ubsan", "Debug", "undefined"),
                    ("asan", "Debug", "address,undefined"),
                    ("tsan", "Debug", "thread")):
                build = cls.work / name
                flags = "-ivfsoverlay " + shlex.quote(str(cls.overlay))
                if sanitizer:
                    flags += (f" -fsanitize={sanitizer} -fno-sanitize-recover=all"
                              " -fno-omit-frame-pointer")
                configure = [
                    "cmake", "-S", str(ABI), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_OSX_ARCHITECTURES=arm64",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    f"-DCMAKE_BUILD_TYPE={mode}", f"-DCMAKE_C_FLAGS={flags}",
                    f"-DCMAKE_CXX_FLAGS={flags}", f"-DSDL2_DIR={sdl}",
                    f"-DOPENSSL_ROOT_DIR={openssl}",
                    f"-DCMAKE_PREFIX_PATH={Path(sdl).parents[2]};{openssl}",
                ]
                if sanitizer:
                    configure.append(f"-DCMAKE_EXE_LINKER_FLAGS=-fsanitize={sanitizer}")
                cls.run_setup(configure, name + " configure")
                targets = (TSAN_TARGETS if name == "tsan" else
                           ASAN_TARGETS if name == "asan" else TARGETS)
                cls.run_setup(["cmake", "--build", str(build), "--target",
                               *targets, "--parallel", "3"], name + " build")
                cls.builds[name] = build
        except Exception:
            shutil.rmtree(cls.work, ignore_errors=True)
            raise

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    @classmethod
    def run_setup(cls, command, label):
        result = subprocess.run(command, env=cls.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=600)
        if result.returncode:
            raise RuntimeError(f"{label} failed:\n{result.stdout[-6000:]}")
        return result

    def run_fixture(self, target, configuration):
        result = subprocess.run([str(self.builds[configuration] / target)],
                                env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=90)
        diagnostic = ("undefined behavior reported by sanitizer"
                      if "runtime error:" in result.stdout else
                      "data race reported by sanitizer"
                      if "ThreadSanitizer: data race" in result.stdout else
                      "synthetic behavior assertion or process failure")
        self.assertEqual(result.returncode, 0,
                         f"{configuration} {target}: {diagnostic}")

    def test_numeric_memory_fp_vectors_and_flags(self):
        for configuration in ("release", "ubsan", "asan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_numeric", configuration)

    def test_direct_and_indirect_calls(self):
        for configuration in ("release", "ubsan", "asan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_dispatch", configuration)

    def test_guest_thread_initialization_and_stack_pool(self):
        for configuration in ("release", "ubsan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_threads", configuration)

    def test_utf16_and_atomic_visibility(self):
        for configuration in ("release", "ubsan", "asan", "tsan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_unicode_atomics", configuration)

    def test_compatibility_cxx_linkage(self):
        for configuration in ("release", "ubsan", "asan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_cxx_linkage", configuration)

    def test_audio_reference_count_does_not_wrap(self):
        for configuration in ("release", "ubsan", "asan"):
            with self.subTest(configuration=configuration):
                self.run_fixture("abi_audio_lifetime", configuration)

    def test_trace_writers_with_thread_sanitizer(self):
        self.run_fixture("abi_trace_concurrency", "release")
        self.run_fixture("abi_trace_concurrency", "ubsan")
        self.run_fixture("abi_trace_concurrency", "asan")
        self.run_fixture("abi_trace_concurrency", "tsan")

    def test_guest_thread_address_sanitizer(self):
        self.run_instrumented_runtime("asan")

    def test_guest_thread_thread_sanitizer(self):
        self.run_instrumented_runtime("tsan")

    def run_instrumented_runtime(self, configuration):
        process = subprocess.Popen([str(self.builds[configuration] / "abi_threads")],
                                   env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            output, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                sample = subprocess.run(["sample", str(process.pid), "1", "1"],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        text=True, timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                sample = None
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
            if (sample is not None and sample.returncode == 0 and "dllinit" in sample.stdout and
                    "error_dialog" in sample.stdout and "libSDL2" in sample.stdout):
                self.skipTest("SDL compatibility library blocks at startup under this sanitizer")
            self.fail(f"{configuration} guest-thread fixture timed out outside the known loader issue")
        diagnostic = ("undefined behavior reported by sanitizer"
                      if "runtime error:" in output else
                      "data race reported by sanitizer"
                      if "ThreadSanitizer: data race" in output else
                      "synthetic behavior assertion or process failure")
        self.assertEqual(
            process.returncode, 0,
            f"{configuration} guest threads: {diagnostic}\n{output[-6000:]}")

    def test_unaligned_volatile_access_has_byte_alignment_in_ir(self):
        ir = self.work / "alignment.ll"
        command = [
            self.clang, "-isysroot", self.sdk, "-arch", "arm64", "-std=c11",
            "-O2", "-S", "-emit-llvm",
            "-I", str(TEMPLATE.parent), "-I", str(APPLE), "-I", str(RUNTIME / "src"),
            "-include", str(APPLE / "recomp_compat.h"),
            "-ivfsoverlay", str(self.overlay),
            str(ABI / "alignment_probe.c"), "-o", str(ir),
        ]
        self.run_setup(command, "alignment IR")
        operations = [line for line in ir.read_text().splitlines()
                      if re.search(r"\b(?:load|store) volatile (?:i16|i32|i64|float|double)\b",
                                   line)]
        self.assertGreaterEqual(len(operations), 10)
        self.assertTrue(all(re.search(r"\balign 1(?:,|\s|$)", line)
                            for line in operations), "multibyte guest access is over-aligned")

    def test_overlay_strictly_rejects_unverified_and_unexpected_inputs(self):
        original = TEMPLATE.read_bytes()
        altered = self.work / "altered.h"
        altered.write_bytes(original + b"\n")
        output = self.work / "rejected"
        with self.assertRaises(ValueError):
            self.adapter.prepare(altered, output, template=True)
        self.assertFalse(output.exists())

        fake_manifest = self.work / "synthetic-manifest.json"
        fake_manifest.write_text(json.dumps({
            "final": {"recomp_types.h": hashlib.sha256(original).hexdigest()}
        }))
        with mock.patch.object(self.adapter, "MANIFEST", fake_manifest):
            with self.assertRaises(ValueError):
                self.adapter.prepare(TEMPLATE, output, template=False)
        self.assertFalse(output.exists())

        changed = original.replace(b"#define MEM16(addr)", b"#define MEM16_CHANGED(addr)", 1)
        self.assertNotEqual(changed, original)
        altered.write_bytes(changed)
        with mock.patch.object(self.adapter, "TEMPLATE", altered):
            with self.assertRaises(ValueError):
                self.adapter.prepare(altered, output, template=True)
        self.assertEqual(TEMPLATE.read_bytes(), original)
        self.assertFalse(output.exists())

        adapted = self.overlay.parent / "adapted-abi.h"
        expected = self.adapter.adapt_data(original)
        self.audit.verify_overlay(self.overlay, TEMPLATE, expected, require_private=False)
        try:
            adapted.write_bytes(expected + b"\n")
            with self.assertRaises(self.audit.AuditError):
                self.audit.verify_overlay(self.overlay, TEMPLATE, expected,
                                          require_private=False)
        finally:
            adapted.write_bytes(expected)

    def test_symbol_audit_detects_an_omitted_tracked_hook(self):
        caller = self.work / "audit-caller.c"
        hook = self.work / "audit-hook.c"
        caller.write_text("extern void synthetic_abi_hook(void);\n"
                          "void synthetic_abi_call(void) { synthetic_abi_hook(); }\n")
        hook.write_text("void synthetic_abi_hook(void) {}\n")
        caller_obj, hook_obj = self.work / "audit-caller.o", self.work / "audit-hook.o"
        for source, output in ((caller, caller_obj), (hook, hook_obj)):
            self.run_setup([self.clang, "-isysroot", self.sdk, "-arch", "arm64",
                            "-c", str(source), "-o", str(output)],
                           "synthetic symbol audit")
        candidates = set(self.audit.DEFINITION.findall(hook.read_text()))
        undefined = self.audit.symbols({caller_obj}, True)
        defined = self.audit.symbols({caller_obj}, False)
        self.assertEqual(len(self.audit.unresolved_tracked(undefined, defined, candidates)), 1)
        defined |= self.audit.symbols({hook_obj}, False)
        self.assertEqual(len(self.audit.unresolved_tracked(undefined, defined, candidates)), 0)

    def test_x86_hardware_reference_when_available(self):
        reference = self.work / "x86-reference"
        self.run_setup([self.clang, "-isysroot", self.sdk,
                        "-arch", "arm64", "-arch", "x86_64",
                        "-Wall", "-Wextra", "-Werror", str(ABI / "x86_reference.c"),
                        "-o", str(reference)], "universal reference build")
        architectures = subprocess.check_output(["lipo", "-archs", str(reference)],
                                                text=True).split()
        self.assertTrue({"arm64", "x86_64"} <= set(architectures))
        translated = subprocess.run(["arch", "-x86_64", "/usr/bin/uname", "-m"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    text=True, timeout=10)
        if translated.returncode or translated.stdout.strip() != "x86_64":
            self.skipTest("x86 execution is unavailable; independent ARM expectations still run")
        result = subprocess.run(["arch", "-x86_64", str(reference)], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_z_all_produced_objects_archives_and_executables_have_arm64(self):
        binaries = set(self.work.rglob("*.o")) | set(self.work.rglob("*.a"))
        for configuration in ("release", "ubsan"):
            binaries.update(self.builds[configuration] / target for target in TARGETS)
        binaries.update(self.builds["asan"] / target for target in ASAN_TARGETS)
        binaries.update(self.builds["tsan"] / target for target in TSAN_TARGETS)
        reference = self.work / "x86-reference"
        if reference.exists():
            binaries.add(reference)
        self.assertTrue(binaries)
        for path in sorted(binaries):
            with self.subTest(binary=path.name):
                result = subprocess.run(["lipo", "-archs", str(path)],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        text=True)
                self.assertEqual(result.returncode, 0)
                self.assertIn("arm64", result.stdout.split())


if __name__ == "__main__":
    unittest.main()
