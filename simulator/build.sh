#!/bin/bash
# GrvEP Simulator build script
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug "$@"
make -j$(nproc)
echo ""
echo "Build done: build/grvep_sim"
echo "Run with:  DISPLAY=:1 ./build/grvep_sim"
