# Video application compatibility

[English](VIDEO_APPLICATION_MATRIX.md) | [한국어](ko/VIDEO_APPLICATION_MATRIX.md)

This page separates VA-API driver discovery from complete application playback.
`vainfo` listing a profile proves neither that an application will select it nor
that its output, end-of-stream (EOS), seek, sandbox, and fallback paths work.

The table describes the Debian Trixie ARM64 packages and the checked-in LinDeX
session as of 2026-08-28. Package builds can change; release validation must
record the exact package version.

## What the module installs

Every profile installs FFmpeg, `vainfo`, GStreamer tools, the deprecated
`gstreamer1.0-vaapi` plugin, and `gstreamer1.0-plugins-bad` (including parsers
and the newer `va` plugin). Archcraft Sway also installs mpv. LXQt and XFCE do
not install mpv. No profile installs VLC, Firefox ESR, or Chromium.

The bundled Archcraft Sway archive contains no mpv hardware-decode
configuration. mpv therefore retains its upstream `hwdec=no` default. LinDeX
must not install browser preferences or Chromium feature switches globally.

The checked-in ARM64 driver and helpers are pinned by both `SHA256SUMS`
manifests. They contain the corrected decode lifecycle and bounded repack
resource pool. H.264 Constrained Baseline/Main, HEVC Main, and VP9 Profile 0 are
advertised only after the immutable runtime and live hardware preflight pass.
HEVC/VP9 are public-beta profiles: their 120/120 QTI/PRIME/EOS proof is complete,
but that does not guarantee every player, sink, seek, and sandbox lifecycle.

## Current application matrix

| Consumer | How VA-API is selected | H.264 today | HEVC / VP9 today | Output and zero-copy boundary | Safe release default |
|---|---|---|---|---|---|
| FFmpeg CLI and FFmpeg-based tools | Explicit `-hwaccel vaapi` or an application-created VAAPI hardware device. The CLI does not silently turn it on for normal decode. | The gated reorder candidate completed a 180-frame H.264 Main stream with two B frames at 1280x720 and zero decode errors. This is still a VA-surface/null-output result, not a displayed application lifecycle pass. | HEVC Main and VP9 Profile 0 are advertised as public beta when their exact live preflight succeeds. Each completed 120/120 frames through QTI hardware, PRIME, release, EOS, and codec stop. | Keeping frames as VA surfaces can reach `vaExportSurfaceHandle`. Download to system memory is unavailable for decoded surfaces because decode `vaDeriveImage` and `vaGetImage` are not implemented. | Use explicit VAAPI selection when testing and keep application fallback enabled. |
| mpv | Upstream default is `hwdec=no`. `--hwdec=vaapi,auto` opts into direct VAAPI and retains mpv's software fallback for initialization failure. | Direct VAAPI is a candidate, not an app pass. `--hwdec=vaapi-copy` already failed at the unimplemented download path. | The profiles are now visible after live preflight, but displayed HEVC/VP9 playback and seek remain public test targets. | `vaapi` requires a compatible GPU video output and successful import of the exported NV12 dma-buf/modifier. QCOM UBWC and repacked LINEAR must be tested separately. `vaapi-copy` is not zero-copy and is currently unsupported. | Do not add an mpv.conf override yet. |
| VLC | VLC 3 uses its libavcodec VAAPI/DRM hardware context when hardware decoding is allowed by `avcodec-hw`. VLC is not installed by LinDeX. | Untested. VLC may automatically try an advertised H.264 VLD profile after a user installs it. | Not advertised by the session. | Its direct video-output route and any extraction/download route are distinct. QCOM/LINEAR import, Wayland presentation, seek, and drain have not passed; a download route cannot rely on the missing decode image APIs. | Do not create a VLC preference. Keep software/default behavior until a dedicated run passes. |
| Firefox ESR | The GTK build contains an FFmpeg VAAPI path whose decoded frames are exported as dma-bufs to Gecko/WebRender. Selection also depends on Firefox's hardware-decoding policy and runtime blocklist. Firefox is not installed by LinDeX. | Installed v3.0.3-dev completed 30 seeks and 10 reloads with RDD seccomp mode 2, zero RDD KGSL FDs, `c2.qti.avc.decoder`, 766 decoded/5 dropped frames, 1,757 hardware outputs, 1,758 LINEAR exports, and clean teardown/restart. | HEVC/VP9 are advertised after live preflight, but Firefox playback/seek evidence for those profiles is not yet recorded. | The scoped preconnected-socket gateway keeps QCOM import and the Vulkan repack outside RDD; Firefox receives LINEAR dma-bufs and fences without weakening its sandbox. | Keep the sandbox and scoped launcher. Treat HEVC/VP9 browser playback as public beta. |
| Debian Chromium ARM64 | No runtime flag can enable a compiled-out backend. Debian Trixie's ARM64 Chromium rules set `use_vaapi=false` and use V4L2 instead. Chromium is not installed by LinDeX. | Unavailable in the stock Debian ARM64 package. | Unavailable through ADVC for the same reason. | Chromium's upstream Linux VAAPI path is dma-buf/DRM based, but it is not present in this package. A separately built ARM64 Chromium would require its own sandbox and native-pixmap modifier validation. | Skip. Do not add ineffective or sandbox-weakening command-line flags. |
| GIMP 3 | GIMP uses still-image loaders and GEGL operations; the installed executable does not consume the LinDeX VA-API video driver. | Not applicable. GIMP is not an H.264/HEVC playback or encode acceptance target. | Not applicable. | Image import/export may use CPU or GPU filters, but it does not validate ADVC/MediaCodec video surfaces. | Exclude GIMP from the video-codec matrix. |
| GStreamer `playbin`/`decodebin` | Decoder factories can be auto-plugged by rank. The old `vaapi*` plugin and new `va*` plugin are separate implementations. | An explicit old `vaapih264dec ! video/x-raw(memory:VASurface) ! fakesink` pipeline now reaches downstream/bus EOS and exits cleanly. The failure was undeclared dma-buf cleanup closing GStreamer's FD 0, not software fallback. `playbin`/`decodebin` and the new `vah264dec` path remain untested. | HEVC Main completed 120/120 frames through `c2.qti.hevc.decoder`; VP9 Profile 0 completed 120/120 through `c2.qti.vp9.decoder`. Both used QCOM NV12 input, one Vulkan repack per frame, LINEAR export, explicit EOS, and release. These results authorize public-beta profile advertising after live preflight. | The isolated pass does not prove `playbin` display or seek. New `VAMemory`/DMABuf output and displayed playback still need separate tests. | Keep automatic decoder rank unchanged. Capability advertisement is not automatic application decode proof. |

## Hardware codec test applications

Use these tools from a running LinDeX profile. Record the application version,
codec/profile, output policy (`auto`, `linear`, or `qcom`), resolution, frame
count, seek count, decoded/dropped frames, EOS, and whether teardown left an
orphan broker session.

1. **Inventory — `vainfo`.** Run `vainfo --display drm --device
   /dev/dri/renderD128`. H.264 CB/Main should be present; HEVC Main and VP9
   Profile 0 appear only when their live preflight passed. A listing is not a
   playback result.
2. **Decode transport — FFmpeg.** Run `ffmpeg -hwaccel vaapi
   -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi -i INPUT
   -f null -`. Test separate H.264 Main+B, HEVC Main, and VP9 Profile 0 files.
3. **Displayed playback — mpv.** Run `mpv --hwdec=vaapi,auto INPUT` and exercise
   repeated seek, EOS, replay, window resize, and exit. Do not use
   `vaapi-copy`; decoded system-memory download is not implemented.
4. **Automatic pipeline — GStreamer.** Use `gst-inspect-1.0` to record which
   `vaapi*` or `va*` factories are available, then test `gst-launch-1.0 playbin
   uri=file:///ABSOLUTE/PATH/INPUT`. Also retain an explicit
   VASurface/fakesink run to separate codec/EOS success from Wayland
   sink/import failures.
5. **Sandboxed playback — Firefox ESR.** Open `about:support`, play a local
   H.264/HEVC/VP9 fixture through the LinDeX launcher, seek and reload, then
   record WebRender status plus decoded/dropped counts. Do not disable RDD or
   content sandboxing.
6. **Encode — FFmpeg and OBS.** For FFmpeg use a VAAPI hardware device and the
   `h264_vaapi` or `hevc_vaapi` encoder; in OBS select the advertised VAAPI H.264
   or HEVC encoder. Test 60 seconds, stop cleanly, decode the resulting file,
   and verify frame count, timestamps, and A/V duration.

For Android-side confirmation, capture the LinDeX session log and MediaCodec
metrics showing the selected `c2.qti.*` component. A decoder name, `vainfo`
entry, or smooth-looking video alone is insufficient evidence.

References used for this static classification:

- the [mpv hardware-decoding manual](https://mpv.io/manual/stable/#options-hwdec);
- GStreamer's [hardware decoding and feature-rank guide](https://gstreamer.freedesktop.org/documentation/tutorials/playback/hardware-accelerated-video-decoding.html)
  and the Debian Trixie [gstreamer-vaapi 1.26.2 source](https://sources.debian.org/src/gstreamer-vaapi/1.26.2-1/);
- Firefox's [VAAPI dma-buf frame pool](https://sources.debian.org/src/firefox-esr/140.12.0esr-1~deb13u1/dom/media/platforms/ffmpeg/FFmpegVideoFramePool.h/);
- Chromium's [Linux VA-API documentation](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/gpu/vaapi.md)
  and the Debian Trixie [ARM64 build rules](https://sources.debian.org/src/chromium/150.0.7871.100-1~deb13u1/debian/rules/);
- the Debian Trixie [VLC 3 source package](https://sources.debian.org/src/vlc/3.0.23-0%2Bdeb13u1/).

## Checked-in session comparison

When the outer broker and all immutable artifacts in `SHA256SUMS` pass
preflight, the session exports `LIBVA_DRIVER_NAME=advc` and the H.264 decode
gate. The packaged `advc-vaapi-decode-preflight` then queries the live gateway
for HEVC Main and VP9 Profile 0 independently. Missing PRIME transport,
non-secure QTI hardware identity, or the exact 120/120 token leaves only that
profile unadvertised. Caller-supplied application acknowledgements are ignored.

The remaining release blockers are displayed mpv/playbin/VLC/browser lifecycles
and seek. The 120/120 HEVC and VP9 transport gates do not replace an application
display/seek gate. The explicit old-VAAPI GStreamer H.264
VASurface/fakesink EOS lifecycle now passes, but automatic application paths do
not. The initial environment scrub explicitly clears the
newer `ADVC_VAAPI_ENABLE_HEVC`, `ADVC_VAAPI_ENABLE_VP9`,
`ADVC_VAAPI_GPU_LINEAR_REPACK`, `ADVC_VAAPI_DECODE_OUTPUT`,
`ADVC_VAAPI_ENCODE_INPUT`, and deprecated `ADVC_VAAPI_OUTPUT` variables before the
bounded preflight recomputes the approved subset. It also scrubs the obsolete
application-acknowledgement variables, so inherited development values cannot
open either candidate gate.

The release runtime should be changed only after device lifecycle validation;
this page does not silently change that behavior.

## Fail-closed automatic-selection design

Use a capability gate plus an application test matrix rather than one global
promise:

1. **Runtime-ready gate.** Verify the immutable driver and broker pair, socket,
   render node, procfs FD view, DRM sysfs view, and exact codec capability. This
   HEVC Main or VP9 Profile 0 is advertised as public beta only when its own
   bounded live check passes.
2. **Application evidence.** Track parser, inter-frame stream, output import,
   EOS, seek, teardown, and fallback by application. Missing application
   evidence narrows the public claim but does not manufacture or suppress the
   hardware capability reported by the live preflight.

An application launcher can recompute the runtime-ready gate and set the
minimum environment for its child. If preflight fails, it must remove all ADVC
selectors before starting the application normally. It must not edit global
mpv/VLC/browser preferences. A mid-stream hang cannot be made safe by silently
changing the backing memory of an existing frame; recovery must destroy the
hardware session and let the application restart in software.

Recommended promotion order:

1. FFmpeg H.264 with explicit VA-surface-to-PRIME output and EOS;
2. mpv H.264 direct output, separately for QCOM UBWC and repacked LINEAR;
3. GStreamer old and new VA plugins as distinct rows, with decoder rank left at
   `NONE` until each full run passes;
4. VLC with default fallback behavior;
5. Firefox only after RDD/GPU sandbox and dma-buf modifier validation;
6. stock Debian Chromium ARM64 remains skipped because VA-API is compiled out.

HEVC Main has passed the multi-frame inter-picture VAAPI/PRIME/EOS and
POC-indexed pixel gates and is now a public-beta advertised profile. VP9
Profile 0 has passed the isolated key/inter transport gate and is advertised on
the same basis. Display and seek remain compatibility work. AV1 remains
unavailable through the standard VAAPI
parameter contract described in [Codec and zero-copy status](VIDEO_ZERO_COPY_STATUS.md).

## Zero-copy and EOS claims

- A VA surface is not automatically a presentable dma-buf.
- QCOM UBWC is zero CPU raw-pixel copy only for a consumer that imports the
  exact modifier and layout. It cannot be relabeled LINEAR.
- Repacked LINEAR costs one Vulkan image copy and zero CPU raw-pixel copies. It
  is a compatibility route, not direct decoder allocation.
- `vaapi-copy`, system-memory GStreamer caps, screenshots, filters, or browser
  fallbacks may request `vaGetImage`/`vaDeriveImage`; decoded surfaces do not
  support those calls today.
- A frame-count or `vainfo` success is not EOS proof. Each application gate must
  show its parser EOS, delayed-frame drain, downstream EOS, surface/fence
  release, context destruction, and no orphaned broker session.
