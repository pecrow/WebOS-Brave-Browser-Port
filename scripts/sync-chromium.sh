#!/usr/bin/env bash
set -euo pipefail

workspace="$HOME/lg-tv-brave-v1.97.59"
export PATH="$workspace/tools/node-v24.16.0-linux-x64/bin:$workspace/tools/pnpm/node_modules/.bin:$PATH"
export GIT_TERMINAL_PROMPT=0
cd "$workspace/src/brave"
test "$(node -p "require('./package.json').version")" = "1.97.59"
echo "Initializing pinned Chromium and ARM64 dependencies; log: $workspace/chromium-sync.log"
if ! pnpm run init --target_os=linux --target_arch=arm64 --no-history \
    >"$workspace/chromium-sync.log" 2>&1; then
    tail -n 60 "$workspace/chromium-sync.log"
    exit 1
fi
echo "Pinned Chromium checkout and hooks finished."
