#!/usr/bin/env bash
set -euo pipefail

workspace="$HOME/lg-tv-brave-v1.97.59"
node_version=24.16.0
pnpm_version=11.11.0
brave_version=1.97.59

if [[ -e "$workspace" ]]; then
    echo "Build workspace already exists; inspect it before resuming: $workspace" >&2
    exit 1
fi

mkdir "$workspace"
mkdir "$workspace/tools" "$workspace/downloads" "$workspace/src"
cd "$workspace/downloads"
node_archive="node-v${node_version}-linux-x64.tar.xz"
node_base="https://nodejs.org/dist/v${node_version}"
curl --fail --location --silent --show-error "$node_base/$node_archive" -o "$node_archive"
curl --fail --location --silent --show-error "$node_base/SHASUMS256.txt" -o SHASUMS256.txt
grep "  ${node_archive}\$" SHASUMS256.txt | sha256sum --check -
tar -xJf "$node_archive" -C "$workspace/tools"
export PATH="$workspace/tools/node-v${node_version}-linux-x64/bin:$workspace/tools/pnpm/node_modules/.bin:$PATH"
npm install --prefix "$workspace/tools/pnpm" "pnpm@$pnpm_version" --no-audit --no-fund

git clone --quiet --depth 1 --branch "v$brave_version" \
    https://github.com/brave/brave-core.git "$workspace/src/brave"
cd "$workspace/src/brave"
test "$(node -p "require('./package.json').version")" = "$brave_version"
printf 'Pinned Brave source: '
git rev-parse HEAD
node --version
pnpm --version
pnpm install --frozen-lockfile
echo "Pinned Brave tooling ready; Chromium checkout has not started."
