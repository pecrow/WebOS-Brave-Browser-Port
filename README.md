# Brave for LG webOS

An unofficial Brave browser port for rooted LG TVs, maintained by **Pecrow**.

**Version:** 1.1.0 | **Browser:** Brave 1.97.59 / Chromium 155.0.8059.85

**Tested on:** LG B9 (OLED55B9PSB), webOS 4.10.2, firmware 05.50.00

## What's new

- **Hardware video decoding:** native H.264/AAC playback, including compatible
  HLS streams, for smoother video and lower CPU use on the tested B9.
- **GitHub shortcut:** a permanent icon beside Shields opens this repository in
  a new tab, independently of your homepage settings.
- **Black default theme:** new profiles start with Brave's Ultra Dark theme.

## Features

| Feature | Status | Details |
|---|---|---|
| Websites, tabs, bookmarks, Reading List | Included | Standard browser features retained. |
| Remote pointer, clicks, scroll wheel | Working | Native input handling and popup mouse-capture fixes. |
| On-screen keyboard and Back button | Working | Short Back navigates; hold Back for three seconds to close the app. |
| Audio playback | Working | Patched audio client for webOS. |
| Hardware video decoding | Working | H.264/AAC and compatible HLS streams verified on the B9; other formats may fall back to software or be unsupported. |
| GitHub shortcut | Included | Icon beside Shields opens the repository in a new tab, independently of homepage settings. |
| Black / Ultra Dark theme | Default for new profiles | Existing theme preferences are preserved. |
| Repository homepage | Default for new profiles | Existing home/startup settings are preserved. |
| Shields and ad blocking | Enabled | Public filters and scriptlets; filter downloads run after closing the browser. |
| Extensions and Safe Browsing | Included | Individual extension compatibility is not guaranteed. |
| Memory Saver | Enabled by default | Medium setting with TV-tuned memory budgets. |
| YouTube autoplay | Blocked by default | Manual playback remains available. |
| AI assistants and local AI | Disabled | Removed from this build. |
| Wallet, Rewards, Brave Ads | Disabled | Website ad blocking remains enabled. |
| Brave VPN, Tor, News, Talk, Playlist | Disabled | Optional browser extras removed. |
| PDF viewer and printing | Disabled | Includes print preview. |
| Widevine DRM | Disabled | Streaming services requiring Widevine are unsupported. |
| Browser speech service and background mode | Disabled | Reduced background functionality. |
| Automatic browser engine updates | Disabled | Install a new package to update the browser; ad-block filters update separately. |
| Accelerated browser rendering | Not integrated | Browser UI and page rendering still use software; compatible video uses the native decoder. |

## Installation

Requires a **rooted TV**, the **Homebrew Channel root service**, and sufficient
internal storage. Other TV models have not been verified.

Install **`org.webosbrew.brave.b9_1.1.0_arm.ipk`** from the latest release using
a compatible webOS installer, then launch Brave. The complete package contains
the browser, runtime, display/input host, native video helper, fonts, and initial
ad-block resources. No separate browser payload or USB drive is required.

Allow roughly **1 GB of internal storage**, plus profile/cache space and
temporary installation space. Rooting and Homebrew setup are not included.

## Limitations

- Hardware decoding is verified for H.264/AAC video on the B9. Other formats and
  streaming sites may fall back to software decoding or be unsupported.
- Browser UI and page rendering still use software rendering.
- Widevine-protected streaming is unsupported. Optional features such as Wallet,
  Rewards, VPN, Tor, PDF viewing, and printing are disabled.
- This experimental port runs without Chromium's normal sandbox.
- Ad blocking and extension compatibility vary by site and extension.
- Browser updates require a new package; ad-block filters update separately.

## Building and licensing

See [BUILD.md](BUILD.md) for Linux/WSL2 build instructions and pinned dependencies.
Port sources, patches, and build helpers are included in this repository.

Original integration code is MIT-licensed; see [LICENSE](LICENSE).
Upstream software retains its own licenses and credits; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
