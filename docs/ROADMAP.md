# Roadmap

[English](ROADMAP.md) | [한국어](ko/ROADMAP.md)

## Short closeout list

1. Final tagged ZIP and three-profile lifecycle/hotplug matrix.
2. Packaged player/browser decode-seek and lightweight/OBS encode proof.
3. USB dock ownership plus long-duration suspend/hotplug leak testing.
4. EGL fallback and additional-device portability evidence.

Core reference-device graphics work--hardware Vulkan, QCOM/UBWC preference and
ordinary fullscreen direct scanout--is no longer an open implementation item.

## Version 3 public release gate

- [x] Preserve av2xn project ancestry and MIT notices under the LinDeX name.
- [x] Use a small DRM lease bridge with unmodified compositor runtimes.
- [x] Keep Android `/system`, `/vendor`, and `/product` outside the write path.
- [x] Provide a rootfs-inclusive KernelSU installation path and WebUI.
- [x] Separate release/no-persistent-log and dev/bounded-log packages.
- [x] Scope process, lease, unplug, and USB-input cleanup to the owned session.
- [x] Provide the three supported profiles: Archcraft Sway Free, LXQt + stock
  labwc, and XFCE + stock labwc.
- [x] Omit chroot PolicyKit agents; expand LXQt into explicit components so
  PCManFM-Qt remains available without the `lxqt-policykit` dependency.
- [x] Package checksum-locked GPL-3.0 Archcraft Sway assets from the official
  public GitHub repositories; keep private or paid archives outside source and
  releases.
- [x] Offer official Dark, official Light, and Pywal Sway modes through WebUI
  Quick start using the pinned public asset aggregate and official theme script.
- [x] Keep unsupported strict codec paths and decode PRIME fail-closed.
- [x] Publish paired English/Korean user and release documentation.
- [ ] Verify clean release and dev ZIPs from the exact tag.
- [ ] Install the exact release ZIP with DP detached and reboot Android normally.
- [ ] Complete the three-profile start/stop/unplug/reconnect device matrix.
- [x] Complete bounded packaged-module FFmpeg/GStreamer H.264/HEVC and OBS
  H.264 texture encode measurements on the reference device.
- [x] Complete bounded explicit `modifier = 0` LINEAR decode/import validation.
- [ ] Complete the USB dock matrix.
- [x] Complete ordinary fullscreen same-lease direct-scanout evidence under the
  exact UBWC-preference gate; compatibility-first `auto` remains LINEAR.
- [ ] Add an opt-in, ID-based Sway/Waybar fullscreen watcher and prove its
  deterministic `dock`/`invisible` transition with same-lease GETFB2. Do not
  use Waybar 0.12's non-idempotent `SIGUSR1` toggle or force scanout over a
  visible bar.

## Codec follow-up

- [x] Pass 120 consecutive QCOM UBWC H.264 decoder PRIME frames plus EOS through
  Turnip content and release-fence validation with no raw-pixel CPU copy.
- [x] Validate the one-copy LINEAR repack + Turnip route with explicit
  `modifier = 0`; the reference decoder did not produce a direct LINEAR buffer.
- Complete HEVC POC-indexed pixel accuracy and packaged player display/seek
  before advertising those broader decode paths.
- Reject ambiguous modifier metadata; accept `0` only as explicitly declared
  DRM LINEAR.
- Repeat malformed-descriptor, disconnect, teardown, and session recreation
  tests on the packaged module.
- Publish zero CPU raw-pixel copy and GPU-work counts separately.

## Reliability gate

Before a production-ready claim, one documented device profile should complete
long-duration desktop use, repeated hotplug, Android display restoration, and
suspend/resume cycles without lease, FD, framebuffer, or GPU-object leaks. The
exact duration and cycle counts must be published with the release evidence.

## Portability

- Add independently tested Qualcomm device profiles beyond Adreno 830.
- Validate KernelSU first; document Magisk and APatch only after separate tests.
- Re-run stock compositor behavior after Debian/wlroots updates.
- Add explicit coverage for Vulkan-unavailable/EGL-available devices.
