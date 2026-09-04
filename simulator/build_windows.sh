#!/bin/bash
# Cross-compiles the GrvEP simulator into a native Windows .exe from Linux, via
# MinGW-w64. Produces a single self-contained GrvEP.exe (statically linked: no
# SDL2.dll/libstdc++-6.dll/libwinpthread-1.dll to ship alongside it, no console
# window) — double-click it on Windows, no install step.
#
# First run bootstraps the cross toolchain + SDL2 dev files into
# .winbuild-toolchain/ (~250MB, gitignored) purely as downloaded/extracted files —
# no `apt install`, no root/sudo needed (uses `apt-get download`, which only fetches
# a .deb to the current directory, plus a plain SDL2 tarball from GitHub). Later
# runs skip the bootstrap and just rebuild.
set -e
cd "$(dirname "$0")"

TC_DIR="$(pwd)/.winbuild-toolchain"
SDL2_VERSION="2.30.0"

if [ ! -x "$TC_DIR/usr/bin/x86_64-w64-mingw32-g++-posix" ]; then
    echo "==> Bootstrapping MinGW-w64 cross toolchain (one-time, no root needed)..."
    mkdir -p "$TC_DIR/_dl"
    (
        cd "$TC_DIR/_dl"
        apt-get download \
            g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix \
            mingw-w64-x86-64-dev mingw-w64-common binutils-mingw-w64-x86-64
        for f in *.deb; do dpkg-deb -x "$f" "$TC_DIR/"; done
    )
    rm -rf "$TC_DIR/_dl"
fi

SDL2_MINGW_DIR="$TC_DIR/SDL2-mingw/x86_64-w64-mingw32"
if [ ! -f "$SDL2_MINGW_DIR/lib/libSDL2.a" ]; then
    echo "==> Downloading SDL2 ${SDL2_VERSION} mingw dev package (one-time)..."
    mkdir -p "$TC_DIR/_sdl"
    curl -sL -o "$TC_DIR/_sdl/sdl2.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
    tar xzf "$TC_DIR/_sdl/sdl2.tar.gz" -C "$TC_DIR/_sdl"
    mkdir -p "$TC_DIR/SDL2-mingw"
    cp -r "$TC_DIR/_sdl/SDL2-${SDL2_VERSION}/x86_64-w64-mingw32" "$TC_DIR/SDL2-mingw/"
    rm -rf "$TC_DIR/_sdl"
fi

BUILD_DIR="build-windows"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/toolchain-mingw64.cmake" \
    -DMINGW_TC_DIR="$TC_DIR" \
    -DSDL2_MINGW_DIR="$SDL2_MINGW_DIR"
cmake --build "$BUILD_DIR" -j"$(nproc)"

cp "$BUILD_DIR/grvep_sim.exe" ./GrvEP.exe
echo ""
echo "Build done: simulator/GrvEP.exe"
echo "Copy that one file to a Windows machine and double-click it — no install, no DLLs needed."
