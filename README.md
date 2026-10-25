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

- Tabs, bookmarks, Reading List, extensions, and Safe Browsing.
- Shields and ad blocking, with filter downloads after closing the browser.
- Remote pointer, scrolling, on-screen keyboard, and Back navigation.
  Hold Back for three seconds to close the app.
- TV-tuned memory settings and Memory Saver enabled by default.
- Repository homepage for new profiles. Existing profiles and settings are preserved.

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
