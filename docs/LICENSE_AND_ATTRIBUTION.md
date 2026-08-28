# License and attribution

[English](LICENSE_AND_ATTRIBUTION.md) | [한국어](ko/LICENSE_AND_ATTRIBUTION.md)

## LinDeX license

LinDeX code is distributed under the [MIT License](../LICENSE). The `LICENSE`
file is the normative legal text. This guide summarizes repository attribution
and does not replace that license.

## Project ancestry

LinDeX derives in part from these MIT-licensed projects by **av2xn**:

- [DOAN](https://github.com/av2xn/DOAN)
- [Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot)

The original `Copyright (c) 2026 av2xn` notice is retained in `LICENSE` and
`NOTICE.md`. LinDeX changes are credited to JHoon and LinDeX contributors.

## Third-party software

LinDeX does not relicense third-party components. Debian packages, Mesa,
wlroots, Wayland, libdrm, libseat, Android interfaces, compositors, and provider
sources keep their upstream licenses. Binary release owners must include or
offer corresponding license/source material whenever those terms require it.

See [THIRD_PARTY.md](../THIRD_PARTY.md) for the component list and upstream
links. A checksum proves file identity; it does not grant a new license.

## Archcraft boundary

The Archcraft Sway Free release profile contains checksum-locked copies of
official public GPL-3.0 files from these Archcraft GitHub sources:

- [`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway),
  commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7` — Sway dotfiles and
  wallpaper;
- [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes),
  commit `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` —
  `Sweet-Ambar-Blue` and `Qogir-Light`;
- [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons),
  commit `1af3af70ccb233bf26f42162f7e65e4a36803667` — `Ars`, `Qogir`, and the
  `Archcraft` fallback;
- [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors),
  commit `8b7e4633cf8e73502f2cfd396d077edf9304c440` — `Sweet` and
  `Qogirr-Dark`, with the latter's
  [corresponding public source](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors).

Those copied files retain GPL-3.0. Their source locks, archive hashes, and
installation paths are recorded under `module/profile-assets/`. LinDeX applies
only a documented Debian/chroot adapter that disables Android-owned power,
internal-brightness and Bluetooth actions plus automatic lock/DPMS; it does not
relicense the visual files. The checksum-locked
`lindex-archcraft-sway-public-assets-v2.tar.gz` is a reproducible LinDeX
aggregate of these public inputs, not an upstream Archcraft distribution
archive. Pywal mode calls the installed official profile script
`~/.config/sway/theme/theme.sh --pywal`. The aggregate physically includes the
Qogirr-Dark corresponding SVG source under
`corresponding-source/Qogirr-Dark/src/cursors` and its GPL-3.0 `COPYING` file.

The Archcraft creator's
[Ko-fi page](https://ko-fi.com/s/10f2e87af3) is listed only as thanks and an
optional way to support the creator. It is not an installation source, a
required purchase, or a license condition. LinDeX does not download or bundle
private/paid Ko-fi archives.

Wayfire, River, and Newm have no current release profile. Their earlier
package-list/provider experiments do not grant an Archcraft identity to a
LinDeX desktop and are not shipped in v3.

## Trademarks and affiliation

LinDeX is an independent project name. Android, Samsung, Qualcomm, Debian,
Archcraft, desktop names, and other marks belong to their respective owners. No
affiliation or endorsement is claimed.
