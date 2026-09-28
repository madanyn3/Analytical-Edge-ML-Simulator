#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Debug -DEDGE_SANITIZE=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/fc_example
cmake -S firmware -B build-arm -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-arm -j2
python3 scripts/record_build.py
