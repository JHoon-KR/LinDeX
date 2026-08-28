# Validation status

[English](VALIDATION_STATUS.md) | [한국어](ko/VALIDATION_STATUS.md)

Last updated: 2026-08-28 (Asia/Seoul)

This page is the public evidence ledger for the v3 release candidate. It is
deliberately shorter than private device worklogs. A row marked pending must
not be promoted to a release claim until the named artifact and device result
are recorded here.

## Reference target

- Device: Samsung Galaxy S25 Edge, `SM-S937N`
- GPU: Adreno 830
- Root manager used for acceptance: KernelSU
- Rootfs: Debian 13 (Trixie), ARM64, `/data/local/debian`
- Physical output: USB-C DisplayPort Alt Mode

This is a device-focused integration. Results do not imply automatic support
for other Qualcomm generations, Android builds, root managers, or docks.

## Release matrix

| ID | Area | Current evidence | Public status | Required closeout |
|---|---|---|---|---|
| V-01 | Source builds and host tests | Current bridge, video, WebUI, package/provider, and lifecycle tests pass | Passed on current candidate | Re-run on exact tag and save CI URL |
| V-02 | Public source boundary | Ignore rules exclude rootfs, downloads, logs, historical experiments, and build trees | Passed by repository audit | Re-run manifest and secret scan on tag |
| V-03 | No Android overlay | Module tree and installed module have no `system/` payload; runtime policy forbids writes to read-only Android partitions | Passed by source, package, and installed-tree checks | Re-check on the release tag |
| V-04 | Release/dev logging | Release routes persistent diagnostic streams away; dev rotates bounded logs. Reproducible v3.0.1 candidates passed verification: release `d62e3fe4…19fbc5` (70,555,862 bytes), dev `d40a9c00…e75cf` (70,555,857 bytes) | Passed by source/package checks; the 64 payload entries outside `module.prop`/`flavor.conf` are byte-identical | Confirm both packaged ZIPs on device |
| V-05 | Install and activation | Release ZIP `2e79f952…12beb` was installed with DP detached and activated by `ksud soft-reboot` on the temporary-root reference phone; all six chroot mounts, release flavor, production broker, 200,600-byte driver, and payload hashes passed after activation. A later source-only session-cleanup change means this is an installed validation snapshot, not the final public ZIP | Passed on the reference device snapshot | Repackage the final source and repeat the public normal-reboot workflow on a persistent-root device |
| V-06 | Archcraft Sway Free | Official public asset source locks/checksums, LinDeX aggregate, Dark/Light/Pywal contract, and Debian safety adapter pass | Pending final device matrix | Start, stop, unplug, reconnect; confirm each appearance, wallpaper, Waybar, and adapted controls |
| V-10 | LXQt + labwc | Explicit component/PCManFM-Qt and no-PolicyKit-agent contracts pass | Pending final device matrix | Start, desktop icons, stop, unplug, reconnect |
| V-11 | XFCE + labwc | Single lease compositor, stock `xinitrc`, panel, desktop icons, dock, and terminal verified on device. The packaged launcher prevents Xfce from starting a second nested/windowed labwc | Partial pass | Repeat stop, forced unplug, and fresh start after physical reconnect from the final ZIP |
| V-12 | Encode dma-buf/AHB | In addition to the prior FFmpeg/GStreamer/OBS matrix, the 2026-08-28 isolated 720p60 audit completed H.264 CB 120/120 at 84.9 fps and HEVC Main 120/120 at 81.1 fps on QTI hardware. Generic upload counted one CPU pixel copy plus one GPU conversion; PRIME/OBS counted zero CPU pixel copies plus one GPU conversion | The Vulkan producer uses identity-checked cached imports, four bounded in-flight slots, and no success-path queue-idle serialization. The EGL dma-buf and Android-local AHB producers now use bounded native-fence caches too. AHB identity is the dynamically resolved API-31 system-wide ID plus an owned buffer reference and exact descriptor; missing native fences, missing ID API, or ID lookup failure deliberately retains the synchronous path. Android NDK ARM64 build, static analysis, API-28 symbol audit, and host 15/15 pass; the final AHB cache has not yet received a device runtime performance pass | Repeat both codecs and OBS from the final release ZIP, run the AHB cache probe on-device, and retain bounded teardown evidence |
| V-13 | Decode byte | Reference-device PASS: 720x360, color format 21/NV12, stride 768, slice height 384, 436,176 bytes, one raw-frame CPU copy | Confirmed reference-device result | Repeat on exact release artifact and record hash/EOS/cleanup |
| V-14 | Decode QCOM UBWC PRIME | The extended isolated matrix completed H.264 Main+B 180/180 at 147.28 fps, HEVC Main+B 120/120 at 118.73 fps, and VP9 Profile 0 120/120 at 108.46 fps with QCOM PRIME, release, EOS and codec stop. H.264 without the exact bounded reorder gate stalled at zero output | H.264 exports the exact validated reorder gate after full preflight. HEVC Main and VP9 Profile 0 are advertised as public beta only after their independent live QTI/PRIME/120-of-120 checks. H.264 High, HEVC Main10 and AV1 remain hidden | Repeat displayed GPU-output playback, seek, and teardown per application from the release ZIP |
| V-15 | Modifier `0` LINEAR | The long runs repacked QCOM modifier `0x0500000000000001` into explicit modifier `0` LINEAR with matching content, destination/source fences, one Vulkan image repack and zero CPU raw-pixel copies. Average repack cost was 1.379–1.738 ms | The final gateway adds a bounded 32-slot source/destination pool, descriptor plus fstat identity cache, lease-token ownership, and release-fence-before-reuse waits. Host 13/13 and two identical ARM64 builds pass; original decoder LINEAR remains unavailable | Repeat the final packaged artifact through displayed playback and seek for each public-beta profile |
| V-16 | Direct scanout | Reference-device PASS with pristine Debian `libwlroots-0.18 0.18.2-3` and Sway `1.10.1-2`: strict UBWC preference produced 88/91 same-lease GETFB2 samples as XB24/QCOM `0x0500000000000001`; three samples were LINEAR entry/exit transitions, and restoring Waybar returned XR24 LINEAR. A separate no-pointer-motion run produced 87/89 XB24/QCOM samples. With Waybar mapped, GETFB2 was exact XR24/modifier `0`: its top-layer buffer makes the scene ineligible before modifier negotiation | The compatibility-first `auto` policy remained XR24 LINEAR for 90/90 samples because it appends the candidate without changing stock wlroots' XR24 choice. This is an expected composition fallback, not Waybar disabling UBWC | Repeat the exact test from the release artifact; optionally validate an ID-based Sway IPC fullscreen bar watcher. Use a versioned wlroots adapter or patch only if compressed always-visible composited-desktop buffers are required |
| V-17 | USB input ownership | Session-scoped implementation and host checks exist | Pending dock matrix | Keyboard, mouse, touchpad, unrelated USB, phone touch, stop, reconnect |
| V-18 | Hot-unplug ownership | Host fixtures pass leader-first orphan cleanup, token validation, first-sample hard disconnect, Type-C-bounded startup grace, and two-sample stable reconnect. On the current reference run, software lease reissue did not recover the black output; physical DP detach/reconnect restored it | Pending final device evidence | Force-unplug each profile, confirm internal Android display remains smooth and no token-owned process/DRM FD survives, then prove either a reliable software retrain or document physical reconnect as the required recovery |
| V-19 | Vulkan-priority preload isolation | Staged runtime v12 resolved property-blob symbols from the real `libdrm.so.2`; its host fixture survived an earlier `dlsym` interposer. On the reference device, video `auto` kept the gateway active while unmodified Sway/wlroots remained on `WLR_RENDERER=vulkan` for 117 seconds, its compositor preload contained no Firefox adapter, no GLES2 retry occurred, and teardown left no gateway/session orphan | Source/staged-device pass; not final installed-ZIP evidence | Install the release candidate, reboot through the public workflow, repeat Sway start/stop, then complete Firefox playback/seek acceptance separately |

V-07 through V-09 are retired IDs. Their former Wayfire, River, and Newm
candidates are not v3 release profiles.

## Practical remaining work

The hardware Vulkan, QCOM/UBWC preference, ordinary fullscreen direct-scanout,
and isolated zero-CPU-pixel-copy codec paths are implemented and have bounded
reference-device evidence. Vulkan-priority startup also remains stable with the
codec gateway enabled now that Firefox preload adapters are process-scoped.
The remaining release work is narrower:

1. build and install the exact tagged release package, then repeat all three
   profile start/stop/forced-unplug/reconnect cases;
2. complete displayed player/browser playback and seek plus lightweight and OBS
   encode tests from that package;
3. finish the USB dock ownership matrix and long-duration suspend/hotplug leak
   test; and
4. validate the EGL fallback and additional Qualcomm devices before making a
   portable rather than reference-device claim.

An optional Waybar fullscreen watcher is an optimization for bar-visible
composition, not a blocker for the already proven ordinary fullscreen direct
scanout path. A cooled 1.2 GHz versus thermally capped 607 MHz vkmark rerun is a
performance-baseline closeout, not an implementation prerequisite.

## Update rule

When closing a pending row, record:

1. exact Git commit and module ZIP SHA-256;
2. build flavor;
3. device model and non-unique software identifiers;
4. bounded test inputs and acceptance criteria;
5. measured result, including failures and fallbacks; and
6. whether the evidence came from the packaged module or an isolated
   development probe.

Do not publish a device serial, authentication material, full Android log, or
local absolute path. Isolated-probe evidence must remain labeled as such. The
V-13 through V-15 results above are reference-device path evidence; the exact
tagged release artifact record remains a separate closeout item.
