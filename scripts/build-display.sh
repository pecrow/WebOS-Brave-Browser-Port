#!/usr/bin/env bash
set -euo pipefail
project="${1:-$PWD/brave-webos}"
workspace="$HOME/lg-tv-brave-v1.97.59"
sdk="$workspace/tools/arm-webos-linux-gnueabi_sdk-buildroot"
cmake -S "$project" -B "$workspace/display-build" \
    -DBUILD_TESTING=OFF -DBUILD_DISPLAY_HOST=ON \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER="$sdk/bin/arm-webos-linux-gnueabi-gcc" \
    -DSDL3_DIR="$workspace/sdl3-webos/lib/cmake/SDL3" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$workspace/display-build" --parallel 2
