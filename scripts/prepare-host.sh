#!/usr/bin/env bash
set -euo pipefail

if [[ $(id -u) != 0 ]]; then
    echo "Run build-host preparation as root inside the selected WSL distribution." >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    cmake ninja-build pkg-config curl ca-certificates unzip xz-utils \
    patchelf python3-venv m4

echo "Build-host compiler and packaging tools ready."
