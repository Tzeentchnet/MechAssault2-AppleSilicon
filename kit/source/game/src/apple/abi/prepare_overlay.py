#!/usr/bin/env python3
"""Prepare a verified, private Clang VFS overlay for the guest ABI header."""

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import sys

ROOT = Path(__file__).resolve().parents[6]
TEMPLATE = ROOT / "kit/toolkit/templates/runtime/recomp_types.h"
MANIFEST = ROOT / "kit/patches/manifest.json"

REPLACEMENTS = (
    ("#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))",
     "#define MEM16(addr)  (*(volatile ma2_guest_u16 *)XBOX_PTR(addr))"),
    ("#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))",
     "#define MEM32(addr)  (*(volatile ma2_guest_u32 *)XBOX_PTR(addr))"),
    ("#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))",
     "#define SMEM16(addr) (*(volatile ma2_guest_i16 *)XBOX_PTR(addr))"),
    ("#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))",
     "#define SMEM32(addr) (*(volatile ma2_guest_i32 *)XBOX_PTR(addr))"),
    ("#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))",
     "#define SMEM64(addr) (*(volatile ma2_guest_i64 *)XBOX_PTR(addr))"),
    ("#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))",
     "#define MEMF(addr)   (*(volatile ma2_guest_f32 *)XBOX_PTR(addr))"),
    ("#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))",
     "#define MEMD(addr)   (*(volatile ma2_guest_f64 *)XBOX_PTR(addr))"),
    ("#define RECOMP_FP_PC(x) ((g_fp_control_word & 0x300u) ? (double)(x) : recomp_fp_round24(x))",
     "#define RECOMP_FP_PC(x) ((g_fp_control_word & 0x300u) ? (double)(x) : "
     "ma2_abi_fp_round24((double)(x), g_fp_control_word))"),
    ("#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)",
     "#define RECOMP_FCMP(a, b) ma2_abi_fcmp((double)(a), (double)(b))"),
    ("g_esp < _ap + 4", "!ma2_abi_stack_returned(_ap, g_esp)"),
    ("r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];",
     "r.ud[0] = (uint32_t)((int32_t)a.w[0] * b.w[0]) + "
     "(uint32_t)((int32_t)a.w[1] * b.w[1]);"),
    ("r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];",
     "r.ud[1] = (uint32_t)((int32_t)a.w[2] * b.w[2]) + "
     "(uint32_t)((int32_t)a.w[3] * b.w[3]);"),
)

MACROS = ("MEM16", "MEM32", "SMEM16", "SMEM32", "SMEM64", "MEMF", "MEMD",
          "RECOMP_FP_PC", "RECOMP_FCMP")


def _write_private(path, data):
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    flags |= getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags, 0o600)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as stream:
            fd = -1
            stream.write(data)
    finally:
        if fd != -1:
            os.close(fd)


def adapt_data(data):
    text = data.decode("utf-8")
    for name in MACROS:
        pattern = r"^\s*#\s*define\s+" + re.escape(name) + r"\s*\("
        if len(re.findall(pattern, text, re.MULTILINE)) != 1:
            raise ValueError("ABI header has an unexpected definition count")
    for before, after in REPLACEMENTS:
        if text.count(before) != 1:
            raise ValueError("ABI header does not match the supported adapter")
        text = text.replace(before, after, 1)
    newline = "\r\n" if "\r\n" in text else "\n"
    trace = ("g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \\" + newline +
             "    g_icall_trace_idx++; \\" + newline +
             "    g_icall_count++; \\")
    if text.count(trace) != 5:
        raise ValueError("ABI header has unexpected dispatch trace operations")
    text = text.replace(
        trace,
        "ma2_abi_record_icall(g_icall_trace, &g_icall_trace_idx, &g_icall_count, "
        "ICALL_TRACE_SIZE - 1u, _va); \\",
    )
    return text.encode("utf-8")


def prepare(header, output_dir, template=False):
    data = header.read_bytes()
    if template:
        if data != TEMPLATE.read_bytes():
            raise ValueError("synthetic input does not match the tracked template")
    else:
        expected = json.loads(MANIFEST.read_text(encoding="utf-8"))["final"]["recomp_types.h"]
        if not hmac.compare_digest(hashlib.sha256(data).hexdigest(), expected):
            raise ValueError("private ABI input does not match the shipped manifest")
        if output_dir.resolve().is_relative_to(ROOT):
            raise ValueError("private overlay output must be outside the repository")

    adapted_data = adapt_data(data)
    output_dir.mkdir(parents=True, mode=0o700, exist_ok=True)
    if not template and stat.S_IMODE(output_dir.stat().st_mode) & 0o077:
        raise ValueError("private overlay output directory must be owner-only")
    adapted = output_dir / "adapted-abi.h"
    overlay = output_dir / "overlay.json"
    description = {
        "version": 0,
        "use-external-names": False,
        "roots": [{
            "type": "file",
            "name": str(header.resolve()),
            "external-contents": str(adapted.resolve()),
        }],
    }
    _write_private(adapted, adapted_data)
    _write_private(overlay, json.dumps(description, indent=2).encode("utf-8") + b"\n")
    return overlay


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--template", action="store_true",
                        help="require the exact tracked synthetic template")
    args = parser.parse_args()
    try:
        prepare(args.header, args.output_dir, args.template)
    except (OSError, ValueError, KeyError, UnicodeError, json.JSONDecodeError):
        print("ABI overlay rejected: input, location, or adapter verification failed",
              file=sys.stderr)
        return 1
    print("ABI overlay prepared; source input left unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
