# Profile asset packaging

[English](PROFILE_RUNTIME_PACKAGING.md) | [한국어](ko/PROFILE_RUNTIME_PACKAGING.md)

This page is for release engineering. The current release has three profiles:
Archcraft Sway Free, LXQt + stock labwc, and XFCE + stock labwc. It does not
ship Wayfire, River, or Newm provider runtimes.

## Current packaged assets

LXQt and XFCE use signed Debian Trixie packages and need no profile asset
archive. Archcraft Sway Free requires two bundled, checksum-locked archives:

```text
archcraft-sway-free-e4d0126d.tar.gz
lindex-archcraft-sway-public-assets-v2.tar.gz
```

The first is a complete archive of the official public GPL-3.0
[`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway)
repository at commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`. Its locked
SHA-256 is
`da89184c13bb68affc89b2638efb2d93736dc685e4104f6a5a497c6f9e43dadc`.

The second file is not an upstream Archcraft distribution archive. It is a
reproducible aggregate made by LinDeX from only these pinned public GPL-3.0
source subtrees:

- `Sweet-Ambar-Blue` from
  [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes),
  commit `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`;
- `Ars` from
  [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons),
  commit `1af3af70ccb233bf26f42162f7e65e4a36803667`;
- `Sweet` from
  [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors),
  commit `8b7e4633cf8e73502f2cfd396d077edf9304c440`;
- `Qogir-Light` from `archcraft-themes` at commit
  `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`;
- `Qogir` and its `Archcraft` fallback from `archcraft-icons` at commit
  `1af3af70ccb233bf26f42162f7e65e4a36803667`; and
- `Qogirr-Dark` from `archcraft-cursors` at commit
  `8b7e4633cf8e73502f2cfd396d077edf9304c440`, with its
  [corresponding public cursor source](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors).

Its locked SHA-256 is
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`.
Exact source paths and install paths are recorded in
`module/profile-assets/APPEARANCE_SOURCES.lock`.
The aggregate physically carries the Qogirr-Dark corresponding SVG source at
`corresponding-source/Qogirr-Dark/src/cursors` and its GPL-3.0 `COPYING` file
at `corresponding-source/Qogirr-Dark/COPYING`.

## Acceptance contract

The release package and installer require both archives, their adjacent
`.sha256` files, and the source locks. They reject a missing archive, digest
mismatch, unsafe archive path, symlink escape, special file, incomplete
dotfile tree, or source-lock drift. The Sway profile cannot become ready until
the installed source marker records the expected commit, archive digest,
GPL-3.0 license, and Debian adapter revision.

These are reviewed public GitHub assets, not Ko-fi files. Release engineering
must not substitute a paid/private archive, scrape a user download directory,
or silently generate a plain fallback Sway profile when asset validation
fails. The creator's [Ko-fi page](https://ko-fi.com/s/10f2e87af3) is optional
support only, not an installation source, required payment, or license
condition.

## Installation boundary

The dotfiles are installed under the selected user's Sway configuration. The
LinDeX aggregate installs the exact locked Dark and Light GTK, icon, and cursor
trees under `/usr/share`, with attribution retained under
`/usr/share/doc/lindex`. WebUI Quick start then selects official Dark, official
Light, or Pywal. Pywal uses the installed official script path
`~/.config/sway/theme/theme.sh --pywal`; it does not add an untracked asset
source. LinDeX then applies only the documented Debian/chroot safety adapter:
power is limited to logout, internal brightness and Bluetooth bindings are
disabled, and automatic lock/DPMS is disabled for the leased external output.

No private wlroots, Wayland, libdrm, River, or Newm runtime stack is bundled by
this profile asset path. Old provider builder material that remains in the
development tree is historical and must not be treated as a v3 release input.
