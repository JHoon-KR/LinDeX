# Changelog

[English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md)

## 3.0.4 — 2026-08-28

- Promoted HEVC Main and VP9 Profile 0 decode to public-beta advertisement.
  Each profile remains fail-closed and appears only when the current session
  verifies its exact QTI component, non-secure PRIME transport, and successful
  120/120 long-run token.
- Removed caller-supplied application acknowledgement tokens from codec
  promotion. Display, seek, EOS, sandbox, and teardown results are now tracked
  as application compatibility evidence and cannot forge a hardware profile.
- Added paired English/Korean hardware-codec testing instructions for `vainfo`,
  FFmpeg, mpv, GStreamer, Firefox ESR, and OBS, including the evidence required
  to distinguish inventory, transport, displayed playback, and clean teardown.

## 3.0.3 — 2026-08-28

- Added protocol 1.8 asynchronous LINEAR reservations so H.264 B-frame
  decode can export a dma-buf before reordered QTI output arrives, then bind
  the completed QCOM/UBWC frame through an explicit Vulkan fence without a
  CPU pixel copy.
- Fixed visible 1080-line versus coded 1088-line allocation matching,
  MediaCodec timestamp normalization, reservation-to-output identity
  promotion, and per-frame GPU serialization. Reference-device H.264 B-frame
  60/60, no-B 30/30, HEVC decode, and H.264/HEVC encode smokes passed.
- Split decode-output and encode-input modifier policy into conservative
  `auto|linear|qcom` selectors. Direct QCOM requires descriptor, broker, and
  importer validation; otherwise `auto` uses the explicit LINEAR GPU fallback
  with zero CPU pixel copies.
- Exported the exact asynchronous-decode validation gate from stock profile
  sessions. This closes an activation gap where protocol 1.8 was packaged but
  Firefox and other VA-API clients still selected the synchronous path.
- Replaced the isolated four-frame H.264 reorder bound with the one-frame bound
  required by Firefox ESR's real RDD submission window. The installed
  v3.0.3-dev production gateway completed 30 seeks and 10 document reloads with
  RDD seccomp mode 2, zero RDD KGSL FDs, `c2.qti.avc.decoder`, 1,757 hardware
  outputs, 1,758 LINEAR exports, no timeout or asynchronous failure, and clean
  session teardown/restart.

## 3.0.2 — 2026-08-28

- Fixed the production Android broker launch gate that kept
  `ADVC_FEATURE_DECODE_PRIME` disabled even after the QTI decode, QCOM PRIME,
  Vulkan repack, explicit LINEAR/fence, EOS and teardown matrix had passed.
- Replaced the obsolete validation-only token with an exact versioned release
  token and made the KernelSU broker service override inherited values.
- Added VA-API policy tracing for negotiated transport, ready, hardware and
  advertised codec masks without enabling persistent release logging.
- Preloaded the root-owned ADVC VA driver only in the exact Firefox RDD role,
  before RDD installs seccomp. The sandbox remains enabled and receives no
  KGSL permission; later libva initialization reuses the mapped driver.

## 3.0.1 — 2026-08-28

- Removed the EGL encode producer's per-frame resource and `glFinish()`
  serialization for registered dma-bufs and API-31+ AHardwareBuffers. Bounded
  identity-checked caches now retain the imported image/texture and propagate
  native fences; older or extension-incomplete devices retain the synchronous
  fail-safe path.
- Added a packaged, bounded HEVC Main/VP9 Profile 0 capability preflight. The
  profiles remain unadvertised by default and require both the exact live
  hardware/PRIME transport result and a separate per-device displayed-playback
  and seek acknowledgement.
- Hardened the Firefox RDD repack path with five-second protocol watchdogs,
  stale/reused-FD identity checks, deterministic connection poisoning, stale
  socket recovery, and bounded nonblocking AF_UNIX connects, including listener
  backlog saturation.
- Added 128-cycle lifecycle, 32-cycle abrupt-disconnect, backlog saturation,
  stale socket, FD baseline, timeout, sanitizer, and exact application-gate
  regressions. Firefox's sandbox and preconnected-socket boundary remain intact.

## 3.0.0 — 2026-08-28

- Removed the Vulkan encode producer's per-frame import and whole-queue idle
  bottleneck. Registered dma-bufs now use a bounded identity-checked import
  cache, four in-flight command/fence slots, and swapchain-image-scoped present
  semaphores while preserving explicit acquire/release sync-file ownership and
  fail-closed teardown. The Android ARM64 NDK build and host regressions pass;
  the EGL fallback remains intentionally synchronous pending its own fence-safe
  resource-lifetime implementation.
- Added explicit persistent-encoder EOS finalization: signal once, drain within
  one five-second budget, then unregister surfaces and close the broker. Signal,
  timeout, protocol, and stale-registration failures are fail-closed. Host and
  sanitizer tests pass and the ARM64 VA driver rebuild is reproducible.
- Extended the isolated codec matrix. H.264 Main+B completed 180/180 at
  147.28 fps, HEVC Main+B 120/120 at 118.73 fps, and VP9 Profile 0 120/120 at
  108.46 fps through QCOM/UBWC-to-LINEAR Vulkan repack with zero CPU raw-pixel
  copies. H.264 now receives only the exact validated four-frame reorder gate;
  unsupported profiles remain hidden.
- Recorded 720p60 encode throughput of 84.9 fps for H.264 CB and 81.1 fps for
  HEVC Main. PRIME/OBS input remains zero CPU raw-pixel copy plus one GPU
  conversion; generic FFmpeg upload remains one CPU copy plus one GPU
  conversion.
- Made packaging and verification derive release names and module versions from
  `VERSION`, and consolidated ADVC payload trust into the reviewed checksum
  manifests instead of duplicating stale hashes and byte counts across scripts.

- Fixed the Vulkan-priority startup regression caused by loading Firefox-only
  `dlsym` adapters into the compositor. Firefox EGL/RDD adapters are now
  activated only by the packaged `lindex-firefox` launcher; Sway, labwc, and
  their desktop children receive only the display/input bridge preload set.
- Advanced the immutable display bridge runtime to v12. Its DRM frontend
  resolves `drmModeGetPropertyBlob` and `drmModeFreePropertyBlob` from the
  actual `libdrm.so.2` link map and fails closed if either address resolves
  back to a preload wrapper. A host regression fixture recreates an earlier
  `dlsym` interposer, and the reference device kept unmodified Sway/wlroots on
  `WLR_RENDERER=vulkan` with the codec gateway enabled and no GLES2 retry.
- Made the Archcraft Sway network widget strictly read-only: the profile no
  longer starts `nm-applet` and removes Waybar's `rofi_network` right-click
  action, leaving Android as the sole Wi-Fi policy owner. Added diagnostics
  that distinguish this policy from a Qualcomm CNSS/HAL recovery stall.
- Added the fail-closed Firefox RDD gateway candidate: the RDD sandbox remains
  enabled and uses preconnected sockets, while a root-only helper owns the
  validated QCOM/UBWC-to-LINEAR Turnip repack and explicit fences. The
  RDD-facing VA driver has no Vulkan dependency.
- Rebuilt the Android ADVC broker with concurrent per-client workers and added
  parent-death cleanup to the repack gateway so forced session teardown cannot
  orphan it.
- Prevented Android toybox `tr` from directly retaining `/proc` cmdline or
  environ descriptors. Bounded `dd` snapshots are closed before translation,
  eliminating the previously observed PID-1-owned high-CPU orphan race.
- Restored `wofi`, `kanshi`, and `wlogout` to the Archcraft Sway dependency
  contract for parity with the published cross-distribution install list;
  PolicyKit agents remain intentionally excluded from the root chroot.
- Made `hyprpicker` and `hyprlock` readiness require native dpkg packages and
  added exact-hash removal of obsolete pre-release compatibility wrappers.
- Fixed Archcraft Waybar's missing theme glyph with the installed FontAwesome
  family and bypassed Waybar 0.12's Samsung `charge_now=0` battery calculation
  using a read-only canonical-capacity module; retained the PulseAudio widget.
- Renamed the public project to LinDeX, maintained by JHoon, while preserving
  the MIT notices and ancestry of av2xn/DOAN and av2xn/Magisk-Debian-Chroot.
- Replaced earlier compositor-specific release runtimes with a small,
  immutable-revision DRM lease bridge for unmodified compositors.
- Added a fail-closed standard Vulkan DRM identity layer so validated Qualcomm Turnip
  can match exact KMS nodes in unmodified wlroots while native extension data
  remains untouched and GLES2 remains the fallback.
- Fixed unmodified wlroots Vulkan matching when Android dynamically allocates
  the KGSL character-device major. The v2 session acknowledgement is generated
  from three exact, revalidated device paths instead of a hard-coded major.
- Verified unmodified Sway/wlroots Vulkan output at fullscreen 1920×1080
  Mailbox: vkmark 12,660 versus 4,078 on the preceding GLES2 fallback.
- Scoped synthetic Vulkan DRM identity to the compositor PID, added physical
  device-group dispatch and leak-free bounded dispatch-table lifecycle, and
  marked the inherited lease descriptor close-on-exec. The hardened path kept
  Vulkan active and measured 19,927–20,726 FPS in two bounded fullscreen
  vertex runs.
- Removed Android `/system` overlay installation.
- Added a rootfs-inclusive fresh-install package and destructive full uninstall.
- Documented a normal Android reboot as the public post-install activation
  flow; soft-reboot records apply only to the temporary-root reference phone.
- Added one-button WebUI setup for Archcraft Sway Free and Debian LXQt/XFCE
  with stock labwc.
- Fixed XFCE Wayland startup so the DRM-lease-owning labwc uses Xfce's stock
  labwc configuration and launches Xfce's normal `xinitrc` bootstrap without
  creating a nested compositor. Device validation showed the panel, desktop
  icons, dock, and a native terminal on the single external Wayland output.
- Kept Firefox hardware decode fail-closed after Debian Firefox ESR 140.14
  played the H.264 browser fixture through its software FFmpeg path in the
  headless Wayland check. The B-frame reorder/VUI repair remains a source-level
  candidate until a real DP/WebRender import, seek, drain, and teardown pass.
- Reduced the release surface to those three profiles. Wayfire, River, and
  Newm were removed because no matching official public Archcraft dotfile set
  is packaged for them.
- Removed `lxpolkit`/`xfce-polkit` from the common and profile contracts. The
  LXQt profile expands `lxqt-core` into explicit components so it also omits
  `lxqt-policykit` while retaining the PCManFM-Qt desktop/file manager. Upgrade
  setup purges those obsolete agents and the old metapackage without running
  `autoremove` on the retained desktop components.
- Split release/no-persistent-log and dev/bounded-log flavors.
- Hardened hot-unplug cleanup with a descendant-inherited session token. It
  closes token-owned compositor children even when the launcher leader exits
  first, stops on the first explicit connector disconnect, restricts startup
  grace to a retained Type-C partner, and requires two stable DP+EDID samples
  before automatic reconnect.
- Detached WebUI-started sessions from the Android manager app's freezer
  cgroup before launching the compositor.
- Prevented Waybar from instantiating Android-owned Bluetooth and backlight
  modules; the Bluetooth module could terminate the whole bar when the chroot
  had no BlueZ system bus.
- Bound the retained Waybar battery and network modules to Android's canonical
  `battery` supply and `wlan*` interface, preventing false `0%` and
  `Disconnected` states caused by Samsung auxiliary supplies and policy-route
  `dummy0` selection.
- Documented the upstream distinction between the older promotional gallery
  screenshots and the later public installable `Type-2` Waybar configuration.
- Added active-monitor-session USB keyboard/mouse/touchpad exclusive routing.
- Added EDID-derived modes and explicit experimental 100/120/144 Hz choices.
- Added hardware video backend selection and validated Vulkan dma-buf encode
  with zero CPU pixel copies and one GPU blit into a MediaCodec Surface.
- Kept unsupported codec paths and decode PRIME export fail-closed pending exact
  packaged-module import, content, modifier, and fence validation.
- Confirmed 120 consecutive H.264 QCOM UBWC decode frames plus EOS through
  Turnip content/release-fence validation, and 120-frame explicit modifier-0
  LINEAR Vulkan repack with one GPU copy and zero CPU raw-pixel copies.
- Fixed dma-buf descriptor cleanup so undeclared zero-valued object slots no
  longer close GStreamer's FD 0 bus/poll wakeup descriptor.
- Bundled checksum-locked copies of Archcraft's official public GPL-3.0 Sway
  dotfiles and wallpaper (`archcraft-sway` commit
  `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`) plus its official public
  GPL-3.0 Dark and Light theme, icon, and cursor sources. The appearance
  archive is a reproducible LinDeX aggregate of pinned public files, not an
  upstream Archcraft distribution archive or Ko-fi archive.
- Documented that the pinned repository wallpaper is the sea-and-rock image;
  the dark floral image shown in Archcraft's gallery is present only inside
  composed screenshots and is not treated as a redistributable raw asset.
- Added WebUI Quick start choices for official Dark, official Light, and Pywal
  generation through the installed official Sway script
  `~/.config/sway/theme/theme.sh --pywal`.
- Added a narrow Debian/chroot adapter that preserves the Archcraft visual
  files while disabling Android-owned power, internal-brightness, Bluetooth,
  and automatic lock/DPMS actions.
- Synchronized all three release profile manifests with their installers,
  separated public application lists from transitive runtime libraries, and
  made ready state require package, executable, and Sway asset verification.
- Made the WebUI refresh its profile list from packaged profile metadata while
  retaining the embedded list as an offline fallback.
- Split public user and release documentation into paired English/Korean pages.

Historical v2 compositor experiments remain outside the public v3 source set
under the local ignored `legacy/` and `patches/` directories.
