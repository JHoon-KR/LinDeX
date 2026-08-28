# Offline packages

[English](README.md) | [한국어](README.ko.md)

The installer normally downloads the pinned Mesa archive. For an offline module,
place the first exact file here before packaging:

- `mesa-for-android-container_26.2.0-devel-20260709_debian_trixie_arm64.tar.gz`
- `turnip_26.2.0-devel-20260709_debian_trixie_arm64.tar.gz` (optional
  unpatched-Turnip compatibility override)

The standard archive already includes the patched Turnip Vulkan driver and is
the recommended default. The optional second archive is incomplete by itself;
LinDeX installs it only after the standard archive when the compatibility mode
is selected.

Checksums are still verified by the Debian installer. Large archives are deliberately
not committed to Git.
