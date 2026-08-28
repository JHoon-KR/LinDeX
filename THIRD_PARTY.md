# Third-party components

[English](THIRD_PARTY.md) | [한국어](THIRD_PARTY.ko.md)

LinDeX does not relicense third-party software. Each component remains subject
to its upstream copyright and license.

## Project ancestry

- [av2xn/DOAN](https://github.com/av2xn/DOAN) — MIT License, Copyright
  (c) 2026 av2xn. LinDeX derives its original Android-to-Debian display work
  and project direction from DOAN.
- [av2xn/Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot)
  — MIT License, Copyright (c) 2026 av2xn. LinDeX's rootfs installer and Debian
  chroot carrier derive from this project.

Both upstream MIT notices are retained in this repository's LICENSE and NOTICE.

## Runtime dependencies

- [Mesa for Android container](https://github.com/lfdevs/mesa-for-android-container)
  — Mesa and packaging licenses. LinDeX downloads a pinned release and verifies
  its SHA-256; the archive is not relicensed by LinDeX.
- [Debian](https://www.debian.org/legal/licenses/) — package-specific licenses.
  A rootfs contains many independently licensed Debian packages.
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — MIT License.
  LinDeX v3 uses upstream-compatible wlroots runtime components.
- libdrm, libseat, Wayland, Vulkan, and Linux UAPI interfaces — their respective
  upstream licenses.
- Android NDK MediaCodec, AHardwareBuffer, EGL, and Vulkan interfaces — Android
  platform license terms.

## Desktop profiles

The current release contains three profiles: Archcraft Sway Free, LXQt + stock
labwc, and XFCE + stock labwc. Sway, labwc, LXQt, XFCE, and their dependencies
retain their upstream licenses. Wayfire, River, and Newm are not current release
profiles.

Archcraft Sway Free adapts checksum-locked files from these official public
Archcraft repositories. Each repository declares GPL-3.0:

- [archcraft-sway](https://github.com/archcraft-os/archcraft-sway), commit
  `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`;
- [archcraft-themes](https://github.com/archcraft-os/archcraft-themes), commit
  `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`, component
  `Sweet-Ambar-Blue` and `Qogir-Light`;
- [archcraft-icons](https://github.com/archcraft-os/archcraft-icons), commit
  `1af3af70ccb233bf26f42162f7e65e4a36803667`, components `Ars`, `Qogir`,
  and the `Archcraft` fallback; and
- [archcraft-cursors](https://github.com/archcraft-os/archcraft-cursors), commit
  `8b7e4633cf8e73502f2cfd396d077edf9304c440`, components `Sweet` and
  `Qogirr-Dark`. The latter's corresponding public source is the
  [Qogir cursor tree](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors).

LinDeX applies a Debian safety adapter: Arch-only power, brightness, Bluetooth,
welcome/overview, and automatic DPMS actions are disabled, while equivalent
Debian package names are substituted where needed. That adaptation does not
relicense the original files. Exact source and archive hashes are recorded in
`module/profile-assets/SOURCES.lock` and
`module/profile-assets/APPEARANCE_SOURCES.lock`.
`lindex-archcraft-sway-public-assets-v2.tar.gz` is a reproducible LinDeX
aggregate of those pinned public files, not an upstream Archcraft distribution
archive. Pywal mode executes the installed official profile script
`~/.config/sway/theme/theme.sh --pywal`; it introduces no additional visual
asset license. The aggregate physically includes the Qogirr-Dark corresponding
SVG source under `corresponding-source/Qogirr-Dark/src/cursors` and its
GPL-3.0 `COPYING` file.

The Archcraft Sway creator's
[Ko-fi page](https://ko-fi.com/s/10f2e87af3) is listed as thanks and an optional
support link only. It is not an installation source, required payment, or
license condition. LinDeX does not require or redistribute a private/paid
Ko-fi archive.
