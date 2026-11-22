"""Verify the locked native Python environment without using global packages."""

import importlib.metadata
from pathlib import Path
import platform
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
VENV = ROOT / ".venv"
REQUIREMENTS = ROOT / "tools/apple_silicon/requirements.txt"
PIN = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)==([A-Za-z0-9][A-Za-z0-9._-]*)\s*\\?$")


def main():
    if (VENV.is_symlink() or Path(sys.prefix).resolve() != VENV.resolve()
            or sys.prefix == sys.base_prefix or sys.version_info[:2] != (3, 13)
            or sys.platform != "darwin" or platform.machine() != "arm64"):
        raise SystemExit("Expected the repository's native macOS arm64 Python 3.13 .venv")

    pins = {}
    for line in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "--hash=")):
            continue
        match = PIN.fullmatch(line)
        if not match:
            raise SystemExit(f"Unrecognized requirement: {line}")
        pins[match.group(1)] = match.group(2)
    if not {"capstone", "pytest", "pefile", "numpy", "Pillow"} <= pins.keys():
        raise SystemExit("The Python requirements are incomplete")
    for name, expected in pins.items():
        actual = importlib.metadata.version(name)
        if actual != expected:
            raise SystemExit(f"{name}: expected {expected}, found {actual}")
        print(f"{name}=={actual}")

    import capstone
    import numpy
    import pefile
    import pytest
    import PIL
    from PIL import Image

    for module in (capstone, numpy, pefile, pytest, PIL):
        if VENV.resolve() not in Path(module.__file__).resolve().parents:
            raise SystemExit(f"{module.__name__} was imported from outside .venv")
    library = Path(capstone._cs._name).resolve()
    if Path(capstone.__file__).resolve().parent not in library.parents or not library.is_file():
        raise SystemExit("Capstone did not load the native library bundled with its wheel")
    instructions = list(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(b"\x90\xc3", 0x1000))
    if [insn.mnemonic for insn in instructions] != ["nop", "ret"]:
        raise SystemExit("Native Capstone x86 decoding failed")
    if int(numpy.dot([1, 2, 3], [1, 2, 3])) != 14:
        raise SystemExit("NumPy native execution failed")
    if Image.new("RGB", (1, 1), (1, 2, 3)).getpixel((0, 0)) != (1, 2, 3):
        raise SystemExit("Pillow native execution failed")
    print(f"Python {platform.python_version()}, {platform.system()} {platform.machine()}")
    print(f"Capstone native library: {library.relative_to(VENV.resolve())}")
    print("Capstone x86 decode, NumPy calculation, and Pillow image: OK")


if __name__ == "__main__":
    main()
