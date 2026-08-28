# Installation

[English](INSTALLATION.md) | [한국어](ko/INSTALLATION.md)

> LinDeX v3 is pre-release software. The current reference target is a Samsung
> SM-S937N with Adreno 830, KernelSU, and Debian 13 ARM64. Do not infer support
> for another device until its own validation matrix passes.

## Prerequisites

- ARM64 Android with KernelSU and a module WebUI
- Qualcomm Adreno exposed at `/dev/kgsl-3d0`
- MSM/SDE KMS exposed at `/dev/dri/card0`
- USB-C DisplayPort Alt Mode and a monitor that supplies valid EDID
- Enough free space for the rootfs, packages, Mesa, and user data
- A backup of any existing `/data/local/debian` data you need to keep

The module layout is intended to remain compatible with common root-module
packaging, but the current public installation procedure and acceptance gate
use KernelSU. Other root managers must be documented as unverified until they
are tested separately.

## Fresh install or update

1. Back up data inside the existing Debian chroot.
2. Stop LinDeX from the WebUI if it is running.
3. Physically disconnect the DP cable or USB-C video adapter.
4. Install the rootfs-inclusive LinDeX ZIP in the KernelSU module installer.
5. Read the installer result and stop if a required device or archive check
   fails.
6. Reboot Android normally to activate the root module.
7. Open the LinDeX WebUI and confirm that the reported build flavor and rootfs
   state are expected.
8. Select one profile. For Archcraft Sway Free, also choose **Official Dark**,
   **Official Light**, or **Auto-generated · Pywal**, then press **Start now**.
   First start may install Debian packages, the pinned KGSL Mesa payload, and
   the verified Archcraft Sway assets when that profile is selected.
9. Connect DP only when prompted, or after automatic start has been configured.

A fresh installation validates and extracts `debianfs-arm64.tar.xz` to
`/data/local/debian`. It installs the DRM lease bridge, profile manager, Mesa
setup, session controller, input middleware, release codec services, and the
verified ARM64 ADVC VA-API driver under
`/opt/android-drm-lease-kit/codec/vaapi`. It does not create a `/system`
overlay.

With video acceleration set to **Auto**, LinDeX exposes ADVC to libva and
GStreamer applications only while the packaged driver, production broker
socket, `/dev/dri/renderD128`, `/proc/self/fd`, and `/sys/class/drm/renderD128`
are all available. If any gate fails, the variables remain unset and the
desktop starts without ADVC advertisement. **Disabled** always leaves them
unset.

## First profile setup

The WebUI exposes exactly Archcraft Sway Free, LXQt + stock labwc, and XFCE +
stock labwc. Every profile receives the common base dependencies, including
the codec and graphics runtime, but LinDeX does not add a PolicyKit agent to
the root chroot. The same base transaction installs FFmpeg, `vainfo`, the GStreamer
VA-API plugin, tools, and the Bad plugin set that provides codec parsers such
as `h264parse`; this prevents a healthy ADVC driver from being hidden by a
missing parser. Packages are installed from signed Debian repositories. The Sway
profile additionally verifies and installs the bundled official public
GPL-3.0 Archcraft dotfiles and wallpapers plus Dark/Light GTK, icon, and cursor
assets at the pinned commits documented in [Profiles](LINDEX_PROFILES.md).
LinDeX packages the appearance set as a reproducible aggregate; it is not an
upstream Archcraft distribution archive or Ko-fi archive. The Pywal choice
uses `~/.config/sway/theme/theme.sh --pywal`. Wayfire, River, and Newm are not
release choices. Setup is complete only after package, command, asset, and
checksum verification succeeds.

Do not close the WebUI or attach DP while the first profile shows a package or
provider installation stage. If setup fails, record the short WebUI error and
use a dev build only when diagnostics are needed.

## Mesa

The default setup downloads the pinned KGSL-capable Mesa archive from
[`lfdevs/mesa-for-android-container`](https://github.com/lfdevs/mesa-for-android-container)
and verifies its SHA-256. A failed download or digest check stops without
replacing the active Mesa files. The pinned fork uses the Android Gallium loader
name `kgsl`; do not substitute the upstream DRM `msm` loader for KGSL rendering.

The recommended **KGSL + patched Turnip** mode installs only that standard
archive: it already contains both the KGSL OpenGL/GLES path and the patched
Turnip Vulkan driver. **KGSL + unpatched Turnip** is a compatibility option for
devices on which the standard Turnip fails; it installs the standard archive
first and then applies the separate unpatched-Turnip override. Vulkan probing
still falls back to hardware KGSL GLES when no working hardware Vulkan renderer
is available.

## Verify the installation

After setup, confirm all of the following before calling the installation good:

- the WebUI reports the expected `release` or `dev` flavor;
- the selected profile is ready and the bridge payload is verified;
- DP state changes from detached to connected only after a valid EDID appears;
- start, stop, forced cable removal, and reconnect create clean new sessions;
- Android `/system`, `/vendor`, and `/product` remain untouched;
- release mode creates no persistent setup, session, or codec log;
- the selected USB input mode releases devices when the session stops.

The complete three-profile matrix and the still-disabled decoded DRM PRIME
paths remain release gates. Bounded packaged-module application encode has
already passed on the reference device. See
[Validation status](VALIDATION_STATUS.md).

## Removal

Module removal stops the active session, closes its lease, unmounts nested
chroot mounts, and removes both `/data/local/debian` and
`/data/adb/debian-drm-lease-kit`. This intentionally deletes user data inside
the chroot. Back it up first.

## Source builds

The Git repository does not contain the Debian rootfs or downloaded Mesa
archive. The packager takes an explicitly supplied, reviewed rootfs carrier and
creates separate release and dev ZIPs:

```powershell
./scripts/package-v3-module.ps1 `
  -BaseModuleZip C:\path\to\debian_chroot.zip `
  -Flavor release

./scripts/verify-v3-module.ps1 `
  -ModuleZip ./dist/LinDeX-v3.0.4.zip `
  -ExpectedFlavor release
```

Publication requires the full [release checklist](RELEASING.md), not only a
successful archive build.
