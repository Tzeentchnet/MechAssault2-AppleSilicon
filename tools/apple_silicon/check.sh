#!/usr/bin/env bash
# Check native dependencies and focused tests against a pristine upstream checkout.
set -euo pipefail

if [ "$#" -ne 1 ] || [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    echo "Usage: tools/apple_silicon/check.sh /path/to/xboxrecomp-v0.18.0"
    if [ "$#" -eq 1 ]; then exit 0; else exit 2; fi
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
VENV="$ROOT/.venv"
PYTHON="$VENV/bin/python"
UPSTREAM_SHA=37b61d589168adee3680fabd51805ee81427bf07

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo "This check requires a native macOS arm64 shell." >&2
    exit 1
fi
for command in brew cmake ctest git pkg-config lipo; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required tool is missing: $command" >&2
        exit 1
    fi
done
if [ -L "$VENV" ] || [ -L "$VENV/.tmp" ] || [ ! -x "$PYTHON" ]; then
    echo "Run tools/apple_silicon/setup.sh with an unlinked repository .venv first." >&2
    exit 1
fi
if [ ! -d "$1" ]; then
    echo "Upstream checkout does not exist: $1" >&2
    exit 1
fi
UPSTREAM="$(cd "$1" && pwd -P)"
case "$UPSTREAM/" in
    "$ROOT/"*) echo "The upstream checkout must be outside this repository." >&2; exit 1 ;;
esac
UPSTREAM_ROOT="$(git -C "$UPSTREAM" rev-parse --show-toplevel)"
if [ "$(cd "$UPSTREAM_ROOT" && pwd -P)" != "$UPSTREAM" ] ||
        [ "$(git -C "$UPSTREAM" rev-parse HEAD)" != "$UPSTREAM_SHA" ]; then
    echo "Expected the root of a separate xboxrecomp v0.18.0 checkout at $UPSTREAM_SHA." >&2
    exit 1
fi
if [ -n "$(git -C "$UPSTREAM" status --porcelain --untracked-files=normal)" ]; then
    echo "The upstream checkout must be clean to validate the pinned revision." >&2
    exit 1
fi

unset PYTHONHOME PYTHONPATH PYTHONOPTIMIZE PYTEST_ADDOPTS
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1 PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
mkdir -p "$VENV/.tmp"
export TMPDIR="$VENV/.tmp"
"$PYTHON" "$ROOT/tools/apple_silicon/check_python.py"
"$PYTHON" -m pip --isolated check

SDL="$(brew --prefix sdl2-compat)"
EPOXY="$(brew --prefix libepoxy)"
OPENSSL="$(brew --prefix openssl@3)"
for prefix in "$SDL" "$EPOXY" "$OPENSSL"; do
    if [ ! -d "$prefix/lib/pkgconfig" ]; then
        echo "Homebrew dependency is not installed: $prefix" >&2
        exit 1
    fi
done
export PKG_CONFIG_PATH="$SDL/lib/pkgconfig:$EPOXY/lib/pkgconfig:$OPENSSL/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$SDL:$EPOXY:$OPENSSL${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
for pair in "sdl2:$SDL" "epoxy:$EPOXY" "openssl:$OPENSSL"; do
    name="${pair%%:*}"
    prefix="${pair#*:}"
    found="$(pkg-config --variable=pcfiledir "$name")"
    expected="$(cd "$prefix/lib/pkgconfig" && pwd -P)"
    if [ ! -d "$found" ] || [ "$(cd "$found" && pwd -P)" != "$expected" ]; then
        echo "pkg-config resolved $name outside the requested Homebrew formula: $found" >&2
        exit 1
    fi
    echo "pkg-config $name: $(pkg-config --modversion "$name")"
done

WORK="$ROOT/_work/apple-silicon"
DEPS_BUILD="$WORK/dependency-probe"
NATIVE_CMAKE_ARGS=(
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL2_DIR="$SDL/lib/cmake/SDL2" \
    -DOPENSSL_ROOT_DIR="$OPENSSL" -DOPENSSL_INCLUDE_DIR="$OPENSSL/include" \
    -DOPENSSL_SSL_LIBRARY="$OPENSSL/lib/libssl.dylib" \
    -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL/lib/libcrypto.dylib"
)
cmake -U '*SDL2*' -U '*EPOXY*' -U '*OPENSSL*' -S "$ROOT/tools/apple_silicon/probe" -B "$DEPS_BUILD" \
    "${NATIVE_CMAKE_ARGS[@]}"
cmake --build "$DEPS_BUILD"
lipo -verify_arch arm64 "$DEPS_BUILD/apple_silicon_dependencies"
"$DEPS_BUILD/apple_silicon_dependencies"
ctest --test-dir "$DEPS_BUILD" --output-on-failure --no-tests=error

cmake -U '*SDL2*' -U '*EPOXY*' -U '*OPENSSL*' -S "$ROOT/kit/source/runtime" -B "$WORK/local-runtime" \
    "${NATIVE_CMAKE_ARGS[@]}"

export PYTHONPATH="$ROOT/tools/apple_silicon"
"$PYTHON" -m pytest -q -ra -p no:cacheprovider -p no_skips \
    "$ROOT/tools/apple_silicon/test_no_skips.py"
(
    cd "$UPSTREAM"
    "$PYTHON" -m pytest -q -ra -p no:cacheprovider -p no_skips \
        tools/disasm/test_decode_at.py \
        tools/recomp/test_lifter_bit_scan.py \
        tools/fusion/test_signatures.py \
        tools/fusion/test_svod.py
)

MMIO_BUILD="$WORK/mmio-decode-a64"
cmake -S "$UPSTREAM/tests/mmio_decode_a64" -B "$MMIO_BUILD" -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$MMIO_BUILD"
lipo -verify_arch arm64 "$MMIO_BUILD/mmio_decode_a64_test"
# Run directly as well: CTest treats the upstream test's exit 77 as a skip/success.
"$MMIO_BUILD/mmio_decode_a64_test"
ctest --test-dir "$MMIO_BUILD" --output-on-failure --no-tests=error

INPUT_BUILD="$WORK/input-map"
cmake -S "$UPSTREAM/tests/input_map" -B "$INPUT_BUILD" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_CXX_STANDARD=17
cmake --build "$INPUT_BUILD"
lipo -verify_arch arm64 "$INPUT_BUILD/input_map_test"
lipo -verify_arch arm64 "$INPUT_BUILD/input_map_cpp"
ctest --test-dir "$INPUT_BUILD" --output-on-failure --no-tests=error

echo "Native dependency and upstream checks passed."
