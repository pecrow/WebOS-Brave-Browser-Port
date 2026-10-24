#!/usr/bin/env bash
set -euo pipefail

workspace="$HOME/lg-tv-brave-v1.97.59"
destination="$workspace/sdl3-webos"
archive="$workspace/downloads/SDL3-3.4.16-webos.5.tar.gz"
if [[ -e "$destination" ]]; then
    echo "Display runtime exists; inspect before resuming: $destination" >&2
    exit 1
fi
curl --fail --location --silent --show-error \
    https://github.com/webosbrew/SDL-webOS/releases/download/release-3.4.16-webos.5/SDL3-3.4.16-webos.5.tar.gz \
    -o "$archive"
echo "e3e99a88cdd88b246d68c3cea42dd0f7c4ac635563ed5352d3bf1a725dc3ac4b  $archive" |
    sha256sum --check -
mkdir "$destination"
tar -xzf "$archive" -C "$destination"
echo "Pinned ARM32 SDL display runtime ready."
