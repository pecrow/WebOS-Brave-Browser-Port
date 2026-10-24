#!/usr/bin/env bash
set -euo pipefail

workspace="$HOME/lg-tv-brave-v1.97.59"
archive="$workspace/downloads/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz"
sdk="$workspace/tools/arm-webos-linux-gnueabi_sdk-buildroot"
if [[ -e "$sdk" ]]; then
    echo "Display toolchain exists; inspect before resuming: $sdk" >&2
    exit 1
fi
curl --fail --location --silent --show-error \
    https://github.com/cscd98/buildroot-nc4/releases/download/webos-c14c7bd/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz \
    -o "$archive"
echo "61ada10c6b2b94510968b99137762cbd7a7956b4bdc4e456792ae8bbe000410e  $archive" | sha256sum --check -
tar -xzf "$archive" -C "$workspace/tools"
"$sdk/relocate-sdk.sh"
"$sdk/bin/arm-webos-linux-gnueabi-gcc" --version | head -n 1
echo "Compatible ARM32 display-host toolchain ready."
