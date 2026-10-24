# Building Brave for webOS

Use Ubuntu 24.04 on x86-64 Linux or WSL2, with substantial free disk space for
the upstream browser checkout and build. Run commands from the repository root.
The scripts refuse to overwrite existing setup/staging directories.

## Prepare

```bash
sudo bash scripts/prepare-host.sh
bash scripts/bootstrap.sh
bash scripts/sync-chromium.sh
bash scripts/prepare-runtime.sh "$PWD"
bash scripts/prepare-display-toolchain.sh
bash scripts/prepare-display-runtime.sh
export PATH="$HOME/lg-tv-brave-v1.97.59/tools/node-v24.16.0-linux-x64/bin:$PATH"
npm ci
```

## Compile and package

```bash
python3 scripts/build-port.py --full
python3 scripts/build-pulse.py
bash scripts/build-display.sh "$PWD"
python3 scripts/package-app.py \
  --output-dir "$HOME/brave-webos-release-1.0.0"
python3 scripts/verify-package.py \
  "$HOME/brave-webos-release-1.0.0/org.webosbrew.brave.b9_1.0.0_arm.ipk"
```

The complete IPK and its SHA-256 file are written to the selected output
directory. Use a new output directory for another packaging attempt; preserve
previous builds. Use the full package, not the historical USB/internal launcher
packages.

Versions and toolchain hashes are recorded in `build-config.json`.
`gn-args.json` selects the TV feature set; `patches/` contains the required
browser and audio changes. SDL is fetched from its pinned upstream release.
Packaging generates a clean profile seed from settings and public filters,
never from a personal TV profile. Filter and CA downloads require internet
access during packaging; source URLs/checksums are included in the payload.

## Checks

```bash
npm run test:adblock
npm run test:profile
cmake -S . -B "$HOME/brave-webos-protocol-tests" -DBUILD_DISPLAY_HOST=OFF
cmake --build "$HOME/brave-webos-protocol-tests"
ctest --test-dir "$HOME/brave-webos-protocol-tests" --output-on-failure
```

The archive verifier checks file hashes, bundled dependencies, permissions,
release metadata, initial settings, and absence of personal profile/session
files. Hardware installation and GUI/audio/input checks still require a
compatible rooted TV. A clean upstream rebuild has not yet been repeated for
the 1.0.0 packaging changes.
