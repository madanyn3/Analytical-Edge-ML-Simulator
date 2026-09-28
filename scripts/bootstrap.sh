#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .deps
sha=13c97dbb6f781d4aab38ed34e6e441f42b79aff4
if [ ! -d .deps/CMSIS-NN/.git ]; then
  git clone --no-checkout https://github.com/ARM-software/CMSIS-NN.git .deps/CMSIS-NN
fi
git -C .deps/CMSIS-NN checkout --detach "$sha"
test "$(git -C .deps/CMSIS-NN rev-parse HEAD)" = "$sha"
archive=arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz
if [ ! -f ".deps/$archive" ]; then
  curl --fail --location --retry 2 "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/$archive" -o ".deps/$archive.partial"
  mv ".deps/$archive.partial" ".deps/$archive"
fi
echo "597893282ac8c6ab1a4073977f2362990184599643b4c5ee34870a8215783a16  .deps/$archive" | sha256sum --check
if [ ! -f .deps/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/.extracted ]; then
  tar -xf ".deps/$archive" -C .deps
  touch .deps/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/.extracted
fi
