#!/usr/bin/env python3
import argparse
import errno
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    compile_result = subprocess.run(
        [
            args.compiler,
            "-arch",
            "x86_64",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(args.source),
            "-o",
            str(args.output),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if compile_result.returncode != 0:
        sys.stderr.write(compile_result.stderr)
        return 1

    try:
        return subprocess.run([str(args.output)], check=False).returncode
    except OSError as error:
        if error.errno in (errno.ENOEXEC, 86):
            print("SKIP: x86_64 execution is unavailable")
            return 77
        raise


if __name__ == "__main__":
    raise SystemExit(main())
