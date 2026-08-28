# LinDeX profiles

[English](LINDEX_PROFILES.md) | [한국어](ko/LINDEX_PROFILES.md)

## Release profile list

The WebUI exposes exactly three profiles:

| Profile ID | Display name | Runtime source | Appearance |
|---|---|---|---|
| `sway` | Archcraft Sway Free · official public dotfiles | Debian Trixie plus pinned Archcraft GitHub sources | Official public Archcraft Sway files with a Debian safety adapter |
| `lxqt` | LXQt + stock labwc | Signed Debian Trixie packages | Upstream defaults |
| `xfce` | XFCE + stock labwc | Signed Debian Trixie packages | Upstream defaults |

Wayfire, River, and Newm are not shipped release profiles. Earlier candidates
listed them from package-composition pages, but LinDeX does not have matching
official public Archcraft dotfile sets to package and verify for those
profiles. Their old provider runtimes and plain fallback configurations are
not part of the current release surface.

## Archcraft Sway Free source lock

The Sway profile is based on Archcraft's official public GitHub repositories,
not on a Ko-fi download. The release locks these GPL-3.0 sources:

| Component | Official source | Pinned commit | Installed content |
|---|---|---|---|
| Sway dotfiles and wallpapers | [`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway) | `e4d0126d7f236fee50a84fbb0e61498dcf5705e7` | Complete public profile archive under the user's Sway config |
| Dark GTK theme | [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes) | `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` | `Sweet-Ambar-Blue` |
| Dark icon theme | [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons) | `1af3af70ccb233bf26f42162f7e65e4a36803667` | `Ars` |
| Dark cursor theme | [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors) | `8b7e4633cf8e73502f2cfd396d077edf9304c440` | `Sweet` |
| Light GTK theme | [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes) | `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` | `Qogir-Light` |
| Light icon themes | [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons) | `1af3af70ccb233bf26f42162f7e65e4a36803667` | `Qogir` with `Archcraft` fallback |
| Light cursor theme | [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors) | `8b7e4633cf8e73502f2cfd396d077edf9304c440` | `Qogirr-Dark`; [corresponding Qogir cursor source](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors) |

The source locks, archive SHA-256 values, and component paths are recorded in
`module/profile-assets/SOURCES.lock` and
`module/profile-assets/APPEARANCE_SOURCES.lock`. The appearance file is
`lindex-archcraft-sway-public-assets-v2.tar.gz`, SHA-256
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`.
It is a reproducible LinDeX aggregate of the pinned public GPL assets above,
not an upstream Archcraft distribution archive. Packaging and installation
verify its adjacent digest and complete source lock. The Qogirr-Dark
corresponding SVG source and license are physically included at
`corresponding-source/Qogirr-Dark/src/cursors` and
`corresponding-source/Qogirr-Dark/COPYING`. The copied files remain GPL-3.0;
LinDeX's MIT license does not relicense them.

This corrects the earlier Ko-fi misunderstanding. LinDeX neither needs nor
searches for a paid/private Archcraft profile archive. It bundles only the
reviewed official public GitHub material listed above. No user download or
manual copy from Android storage is required. The Archcraft Sway creator's
[Ko-fi page](https://ko-fi.com/s/10f2e87af3) is an optional support link only,
not an installation source, required payment, or license condition.

The pinned dotfile repository's actual `files/wallpapers/wallpaper.jpg` is the
sea-and-rock image. Archcraft's public documentation and gallery show a dark
floral wallpaper in composed desktop screenshots, but that raw floral image
is not part of the pinned Sway repository. Gallery screenshots are references,
not installable wallpaper assets, and LinDeX does not crop or redistribute a
wallpaper out of a composed screenshot.

The promotional screenshots and the currently public installable dotfiles are
also different upstream revisions. Archcraft committed the screenshots in
`bc1d170`, when that repository contained screenshots and a README but no
installable `files/waybar` tree. The public Sway configuration was added later
in `6702e80`; the pinned `e4d0126d` revision selects its `Type-2` Waybar layout.
Consequently, a bar that follows the pinned configuration can legitimately
differ from the older gallery bar even when every configuration include is
working. LinDeX does not label a screenshot-only layout as an official
installable profile.

## Sway appearance modes

WebUI Quick start exposes three Sway-only choices and applies the selection
before the next session starts:

- **Official Dark** is the default and uses `Sweet-Ambar-Blue`, `Ars`, and
  `Sweet`.
- **Official Light** uses `Qogir-Light`, `Qogir` with the `Archcraft` fallback,
  and `Qogirr-Dark`.
- **Auto-generated · Pywal** runs the installed official profile script as
  `~/.config/sway/theme/theme.sh --pywal`. It derives colors from images under
  `~/Pictures/wallpapers`; on a fresh profile LinDeX seeds that directory with
  the bundled public Dark and Light wallpapers without overwriting user files.

Pywal changes generated colors; it does not replace the checksum-locked source
assets or bypass their validation. If the pinned `wal` provider or a usable
wallpaper is unavailable, setup fails with a bounded profile/dependency state
instead of silently selecting another appearance.

## Debian/chroot safety adapter

LinDeX preserves the pinned Archcraft visual configuration and changes only
the integrations that are unsafe or unavailable in an Android-owned chroot:

- the power menu can log out of Sway but cannot suspend, hibernate, reboot, or
  power off Android;
- internal-panel brightness bindings are disabled because Android owns that
  display and its brightness policy;
- the Archcraft Bluetooth binding is disabled because Android owns Bluetooth;
- the Waybar Bluetooth and backlight modules are not instantiated. The former
  can abort Debian's Waybar build when no BlueZ system bus exists in the
  chroot, and both controls belong to Android on this device;
- the retained official Waybar battery widget is bound to Android's canonical
  `battery` supply. This avoids
  selecting Samsung auxiliary fuel-gauge supplies with zero capacity or the
  Android policy-routing `dummy0` device and displaying false `0%` or
  `Disconnected` states;
- the network widget and `rofi_network` helper are removed. Sway does not read
  `wlan*`, route, gateway, or NetworkManager state. Android remains the sole
  network policy owner;
- Samsung's canonical battery also reports `charge_now=0` beside a valid
  `capacity` value. Because Waybar 0.12 prioritizes charge counters, LinDeX uses
  a small read-only custom module for the canonical capacity;
- the disconnected PulseAudio widget is replaced with an Android
  `STREAM_MUSIC` widget. It shows the current Android media-volume percentage,
  scrolls one Android step up or down, and toggles between zero and the last
  nonzero value on click. The Android host watcher invokes
  `cmd media_session` with a two-second bound and exchanges only private,
  bounded state/command files through the existing tmpfs-backed
  `/run/android-drm` directory. It neither mounts `cmd`, `/system`, or `/apex`
  into the chroot nor opens an ALSA/PulseAudio output. MPD is not started or
  instantiated in Waybar, and release builds create no persistent volume log;
- `lxpolkit`, `xfce-polkit`, and `lxqt-policykit` are omitted because this root
  chroot has no logind user session for a PolicyKit agent to register;
- `wofi`, `kanshi`, and `wlogout` are installed for parity with Archcraft's
  published cross-distribution dependency list. The pinned free profile still
  uses its Rofi launcher, while Android/WebUI remains the authority for leased
  external-output policy and Android power control;
- `hyprpicker` and `hyprlock` must be native Debian backports packages. During
  an upgrade, LinDeX removes only the exact hashes of its obsolete pre-release
  compatibility scripts so they cannot mask or shadow the real binaries;
- automatic lock and DPMS are disabled for the leased external output;
- Arch-only Sway Overview and welcome/service paths that are unavailable on
  Debian are disabled;
- distro names are mapped where necessary, including `mako` to
  `mako-notifier`, `xorg-xwayland` to `xwayland`, and `python-pywal` to the
  pinned pywal16 provider.

The adapter does not replace the wallpaper, Waybar layout, launcher theme,
colors, GTK theme, icons, or cursor with a plain fallback desktop. A source
marker records the pinned commit, archive digest, GPL-3.0 license, and adapter
revision after installation.

## Shared base dependencies

All three profiles share the common Debian base and omit chroot PolicyKit
agents. The installer keeps public profile applications separate from
transitive runtime libraries so dependency packages are not presented as
extra desktop applications. LXQt uses an explicit component list rather than
`lxqt-core`; this retains `pcmanfm-qt`, the panel, session, runner, themes, and
Qt Wayland integration without pulling `lxqt-policykit`. A profile becomes
ready only after its package, command, asset, and checksum requirements pass;
a stale ready marker is not sufficient.

LXQt and XFCE use stock Debian labwc and do not inherit the Archcraft Sway
dotfiles or appearance assets.

## Desktop icons and file managers

- Archcraft Sway intentionally has no desktop-icon manager. The small icons at
  the upper-left of its bar are Sway workspace buttons, not desktop shortcuts;
  Trash shown in reference screenshots is normally inside an open Thunar
  window. Installing `xfce4-terminal` or `xfce4-settings` does not add desktop
  icons to Sway. The official profile opens Thunar with `Super+Shift+F`.
- LXQt installs `pcmanfm-qt`, which is started as an LXQt session module and
  owns the Wayland desktop surface. Its default desktop shortcuts include Home,
  Trash, Computer, and Network. `gvfs` is included for file/trash integration.
- XFCE's Debian `xfce4` metapackage already installs `xfdesktop4`,
  `xfce4-settings`, and Thunar; LinDeX additionally lists `xfce4-terminal`.
  `xfdesktop4`, not the terminal or settings app, draws XFCE desktop icons.

## One-button setup

For the selected profile, **Start now** performs bounded operations:

1. verify the common base and the selected three-profile contract;
2. install allowlisted signed Debian packages;
3. for Sway, verify and install the pinned official public Archcraft assets,
   then apply the selected Dark, Light, or Pywal mode;
4. install or verify the selected KGSL Mesa payload;
5. verify installed packages, entry-point commands, asset markers, and bridge;
6. start the compositor after physical DP and valid EDID are present.

An asset or checksum failure is reported as `profile-unavailable`; it never
causes a network search for an unofficial archive or a silent minimal-profile
fallback. Existing non-Archcraft user configuration is preserved rather than
silently overwritten.

## Validation status

Host-side package/asset and no-chroot-PolicyKit-agent contracts pass for the
current candidate. The exact packaged-module start, stop, forced
unplug, and reconnect matrix for Archcraft Sway Free, LXQt/labwc, and
XFCE/labwc remains a release gate. See
[Validation status](VALIDATION_STATUS.md).
