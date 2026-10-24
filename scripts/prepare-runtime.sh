#!/usr/bin/env bash
set -euo pipefail

project="${1:-$PWD/brave-webos}"
workspace="$HOME/lg-tv-brave-v1.97.59"
root="$workspace/arm64-runtime"
if [[ -e "$root" && "${2:-}" != --resume ]]; then
    echo "Runtime workspace exists; inspect before resuming: $root" >&2
    exit 1
fi
mkdir -p "$root/apt/lists/partial" "$root/apt/archives/partial" "$root/rootfs"
touch "$root/apt/status"
tr -d '\r' < "$project/runtime-sources.list" > "$root/apt/sources.list"

options=(
    -o "Dir::Etc::sourcelist=$root/apt/sources.list"
    -o "Dir::Etc::sourceparts=-"
    -o "Dir::State::status=$root/apt/status"
    -o "Dir::State::lists=$root/apt/lists"
    -o "Dir::Cache::archives=$root/apt/archives"
    -o "Dir::Cache::pkgcache=$root/apt/pkgcache.bin"
    -o "Dir::Cache::srcpkgcache=$root/apt/srcpkgcache.bin"
    -o "APT::Architecture=arm64"
    -o "APT::Architectures::=arm64"
    -o "APT::Sandbox::User=$(id -un)"
    -o "Debug::NoLocking=1"
)
apt-get "${options[@]}" update -qq
mapfile -t packages < <(tr -d '\r' < "$project/runtime-packages.txt")
apt-get "${options[@]}" --download-only --no-install-recommends -y install "${packages[@]}"
for package in "$root"/apt/archives/*.deb; do
    dpkg-deb --extract "$package" "$root/rootfs"
done
echo "ARM64 runtime dependencies extracted without installing ARM64 packages on the host."
