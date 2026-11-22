#!/usr/bin/env bash
# Create the repository's isolated native Python environment; installs no system packages.
set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "Usage: tools/apple_silicon/setup.sh" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
VENV="$ROOT/.venv"
REQUIREMENTS="$ROOT/tools/apple_silicon/requirements.txt"
PYTHON313="${PYTHON313:-python3.13}"

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo "This setup requires a native macOS arm64 shell." >&2
    exit 1
fi
if ! command -v "$PYTHON313" >/dev/null 2>&1 || [ ! -r "$REQUIREMENTS" ]; then
    echo "Python 3.13 and tools/apple_silicon/requirements.txt are required." >&2
    exit 1
fi

unset PYTHONHOME PYTHONPATH PYTHONOPTIMIZE
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
"$PYTHON313" - <<'PY'
import platform
import sys

if sys.version_info[:2] != (3, 13) or sys.platform != "darwin" or platform.machine() != "arm64":
    raise SystemExit("PYTHON313 must be a native macOS arm64 Python 3.13 interpreter")
PY

if [ -L "$VENV" ]; then
    echo "Refusing to install into a symlinked .venv." >&2
    exit 1
fi
if [ ! -e "$VENV" ]; then
    mkdir "$VENV"
    mkdir "$VENV/.tmp"
    TMPDIR="$VENV/.tmp" "$PYTHON313" -m venv "$VENV"
fi
if [ ! -f "$VENV/pyvenv.cfg" ] || [ ! -x "$VENV/bin/python" ]; then
    echo ".venv exists but is not a usable virtual environment; inspect it before retrying." >&2
    exit 1
fi
if [ -L "$VENV/.tmp" ]; then
    echo "Refusing to use a symlinked .venv/.tmp." >&2
    exit 1
fi
mkdir -p "$VENV/.tmp"
export TMPDIR="$VENV/.tmp"

"$VENV/bin/python" - "$VENV" <<'PY'
from pathlib import Path
import platform
import sys

venv = Path(sys.argv[1]).resolve()
cfg = dict(line.split("=", 1) for line in (venv / "pyvenv.cfg").read_text().splitlines() if "=" in line)
cfg = {key.strip().lower(): value.strip().lower() for key, value in cfg.items()}
if (Path(sys.prefix).resolve() != venv or sys.prefix == sys.base_prefix
        or sys.version_info[:2] != (3, 13) or sys.platform != "darwin"
        or platform.machine() != "arm64" or cfg.get("include-system-site-packages") != "false"):
    raise SystemExit(".venv must be an isolated, native macOS arm64 Python 3.13 environment")
PY

"$VENV/bin/python" -m pip --isolated install \
    --no-input --disable-pip-version-check --no-cache-dir \
    --index-url https://pypi.org/simple --only-binary=:all: --require-hashes --no-deps \
    -r "$REQUIREMENTS"
"$VENV/bin/python" -m pip --isolated check
"$VENV/bin/python" "$ROOT/tools/apple_silicon/check_python.py"
echo "Python setup complete. Run tools/apple_silicon/check.sh with the upstream checkout path."
