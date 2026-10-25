# Licensing and upstream sources

The root `LICENSE` applies to original, independently written integration code
and documentation contributed by Pecrow. It does not relicense upstream code,
patch context, vendored dependencies, downloaded filters, or bundled binaries.
Existing upstream notices and applicable licenses take precedence for those
portions.

| Component | Applicable terms and source |
|---|---|
| Original display/IPC integration, launchers, build helpers, tests, and documentation | MIT; see `LICENSE`. Chromium-derived portions retain Chromium's BSD terms. |
| Brave and modifications to Brave-covered files | MPL 2.0 where applicable; see `licenses/MPL-2.0.txt`. Source: `https://github.com/brave/brave-core`, pinned by the build scripts and release provenance. |
| Chromium and patches to Chromium files | Chromium BSD and each component's own licenses; see `licenses/Chromium-BSD.txt`. Source: `https://chromium.googlesource.com/chromium/src`. |
| Patched PulseAudio client | PulseAudio's existing terms, including LGPL 2.1-or-later for the client; see `licenses/PulseAudio.txt` and `patches/pulse-webos-playback.patch`. Source revision is pinned in `scripts/build-pulse.py`. |
| SDL-webOS native runtime | SDL's zlib license; see `licenses/SDL3.txt`. The release/tag and checksum are pinned in `build-config.json` and `scripts/prepare-display-runtime.sh`. |
| Acorn parser | MIT, copyright its upstream contributors; see `app/acorn.LICENSE`. Version is pinned in `package-lock.json`. |
| Public filters and scriptlets | Their original licenses, not the root MIT license. uBlock-derived resources retain GPL 3.0 terms; see `app/adblock-uBlock.LICENSE`. Download origins are recorded in the package's default ad-block cache. |
| Private runtime libraries and fonts | Individual upstream licenses and copyright notices under `runtime/licenses/` inside the IPK. The release provenance identifies the corresponding distribution packages. |
| CA certificate bundle | Upstream certificate terms; provenance and checksum are in `runtime/ca-source.json` inside the IPK. |

## Obtaining source

This repository contains the webOS port sources, required patches, feature/build
configuration, and build scripts. `BUILD.md` describes how to fetch the pinned
Brave/Chromium sources and rebuild. The 1.1.0 release also provides runtime source
archives and provenance for the bundled libraries, separately from the IPK.
Those archives are for source availability and rebuilding, not installation.

The native video helper calls Starfish/ACB media APIs supplied by the TV firmware.
Those firmware libraries are not included in this repository or the release.

The browser's upstream license information remains available through its own
license/credits resources. No upstream copyright holder or author is replaced
by the port maintainer's name.
