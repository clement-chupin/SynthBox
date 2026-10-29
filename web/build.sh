#!/bin/bash
# GrvEP web (Emscripten/WASM) build script.
# Produces docs/grvep.js — see structure/WEB_SIMULATOR.md.
set -e
cd "$(dirname "$0")"

if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    source "$HOME/emsdk/emsdk_env.sh"
elif ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found and ~/emsdk/emsdk_env.sh doesn't exist — install Emscripten first:" >&2
    echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk" >&2
    echo "  ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest" >&2
    exit 1
fi

mkdir -p build
cd build
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release "$@"
emmake make -j"$(nproc)"

echo ""
echo "Build done: docs/grvep.js"
echo "Serve with: python3 -m http.server 8080 (from the repo root), then open http://localhost:8080/docs/"
