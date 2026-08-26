#!/usr/bin/env bash
# build.sh — compile GrvEP to WebAssembly with Emscripten
# Produces docs/grvep.js (WASM embedded) that docs/index.html loads.
#
# One-time setup (if emsdk not already installed):
#   git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
#   ~/emsdk/emsdk install latest
#   ~/emsdk/emsdk activate latest
#   source ~/emsdk/emsdk_env.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Load Emscripten if emsdk_env.sh exists but emcc is not yet in PATH
if ! command -v emcc &>/dev/null; then
    EMSDK="${EMSDK:-$HOME/emsdk}"
    if [ -f "$EMSDK/emsdk_env.sh" ]; then
        source "$EMSDK/emsdk_env.sh"
    else
        echo "ERROR: emcc not found. Install Emscripten first:"
        echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"
        echo "  ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest"
        echo "  source ~/emsdk/emsdk_env.sh"
        exit 1
    fi
fi

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -G "Unix Makefiles"

emmake make -j"$(nproc)"

echo ""
echo "Build done! docs/grvep.js is ready for GitHub Pages."
echo "Open docs/index.html in a browser to test locally."
