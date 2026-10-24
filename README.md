# Brave for LG webOS

An unofficial native Brave browser port for rooted LG TVs, maintained by **Pecrow**.

**Release package:** 1.0.0  
**Browser baseline:** Brave 1.97.59 / Chromium 155.0.8059.85  
**Tested hardware:** LG OLED55B9PSB (B9), webOS 4.10.2, firmware 05.50.00

> Complete single-IPK installation verified on the B9, including a fresh-profile
> launch with USB and the previous browser payloads hidden.

## Features

These settings describe the current TV-tested baseline.

| Feature | Status | Details |
|---|---|---|
| Websites, tabs, bookmarks, Reading List | Included | Bookmarks and Reading List remain unchanged. |
| Remote pointer, clicks, scroll wheel | Working | Native input handling and popup mouse-capture fixes. |
| On-screen keyboard and Back button | Working | Text input; short Back navigates, holding Back for 3 seconds requests exit. |
| Audio and ordinary web video | Working | Patched audio client; video uses software decoding. |
| Shields and ad blocking | Enabled | Public filters and scriptlet resources; filter updates fetched after closing. |
| Extensions and Safe Browsing | Included | Retained; individual extension compatibility is not guaranteed. |
| Memory Saver | Enabled by default | Medium; initialized automatically for a new profile. |
| YouTube autoplay | Blocked by default | Manual playback remains available. |
| AI assistants and local AI | Disabled | Removed from this build. |
| Wallet, Rewards, Brave Ads | Disabled | Separate from website ad blocking, which remains enabled. |
| Brave VPN, Tor, News, Talk, Playlist | Disabled | Optional browser extras removed. |
| PDF viewer and printing | Disabled | Includes print preview. |
| Widevine DRM | Disabled | Streaming services requiring Widevine are unsupported. |
| Browser speech service and background mode | Disabled | Reduced background functionality. |
| Automatic browser engine updates | Disabled | Engine updates require a new build/install; ad-block filter updates remain enabled. |
| Accelerated browser rendering and platform video decoding | Not integrated | Current browser rendering and video decoding use software paths. |

## Changes for webOS

- Native ARM32 display/input host connected to the ARM64 browser engine.
- Fully internal browser and profile storage; no USB dependency in this variant.
- Patched audio playback, native keyboard, pointer capture, and shutdown handling.
- TV-specific feature trims and build compatibility patches.
- Memory tuning: 96 MiB compositor/tile budget, 16 MiB Skia resource-cache budget,
  and 32 MiB disk cache. These are not a total browser RAM limit.
- Complete public ad-block resources with close-time filter downloads.
- Hold "Back" button on remote for 3 seconds to force close the application. 

## Installation

Requires a rooted, compatible LG TV with the Homebrew Channel root service
and enough free internal storage for the application and profile.
Other TV models have not been verified.

Install **`org.webosbrew.brave.b9_1.0.0_arm.ipk`** using a compatible installer
and launch Brave. This **one complete IPK** contains the engine, private runtime,
patched audio, native display/input host, fonts, and initial public ad-block data.
No separate browser payload, bridge package, USB storage, or manual profile
configuration is required. New profiles receive the TV defaults automatically;
the project repository is the default startup page and Home-button destination.
Existing profiles and their home/startup settings are preserved.

**Storage:** about 300 MiB downloaded and 832 MiB installed, plus profile/cache
storage and temporary installation space.

Root/Homebrew setup is a prerequisite, not something this package installs.

## Rebuilding

Build on Linux or WSL2 using the pinned versions in `build-config.json`.
The repository contains the port in `src/`, launcher and maintenance code in
`app/`, feature configuration in `gn-args.json`, and required changes in
`patches/`. Build and packaging helpers are in `scripts/`; regression tests
are in `tests/`.

Upstream Brave/Chromium sources and dependencies are fetched during setup, not
stored in this repository. Feature notes do not replace the required patches.
Build and package steps are in `BUILD.md`.

## Known limitations

- Experimental B9-tested port, not an official Brave or LG release.
- The browser runs without Chromium's normal sandbox.
- Heavy pages can load slowly, toolbar highlights can lag, and video can drop
  frames. Software decoding does not use the TV's dedicated video decoder.
- Ad blocking is site-dependent; complete YouTube ad coverage is not guaranteed but working.

## Licensing

Original integration code and documentation are MIT-licensed, copyright
**Pecrow**; see `LICENSE`. Brave, Chromium, SDL, PulseAudio, and bundled
libraries/filter resources retain their respective licenses and upstream
credits. See `THIRD_PARTY_NOTICES.md` for scope and source availability.
