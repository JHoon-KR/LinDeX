# Codec and zero-copy status

[English](VIDEO_ZERO_COPY_STATUS.md) | [한국어](ko/VIDEO_ZERO_COPY_STATUS.md)

Last updated: 2026-08-28 (Asia/Seoul)

## Claim boundary

LinDeX has an experimental Android MediaCodec bridge named ADVC. The current
public release claim is deliberately narrow:

- a focused reference-device encode candidate imported a Debian dma-buf with
  Vulkan, performed **zero CPU raw-pixel copies**, and used **one bounded GPU
  blit** into a MediaCodec input Surface;
- byte and Android-local AHardwareBuffer/Surface paths have functional
  development evidence;
- reference-device byte decode passed at 720x360 NV12 with color format 21,
  stride 768, slice height 384, 436,176 output bytes, and one raw-frame CPU copy;
- an isolated decode candidate passed 120 consecutive QCOM UBWC PRIME frames,
  EOS, teardown, Turnip offscreen content validation, and acquire/release-fence
  lifecycle checks with NV12 fourcc `0x3231564e`, modifier
  `0x0500000000000001`, one object, two planes, and no raw-pixel CPU copy;
- the same 120 frames passed a gated QCOM UBWC AHB to explicit
  `modifier = 0` LINEAR dma-buf Vulkan image-to-image repack. It performs one
  GPU repack per frame and zero CPU raw-pixel copies. A source that is already
  explicit LINEAR bypasses this repack;
- the installed exact module passed bounded FFmpeg and GStreamer AVC/HEVC
  encode plus OBS AVC and HEVC texture recording; tagged-release repetition
  remains;
- decode advertisement remains fail closed: H.264 Constrained Baseline/Main,
  HEVC Main, and VP9 Profile 0 may be exposed only after their exact checks.
  HEVC Main and VP9 Profile 0 are public-beta profiles whose live preflight
  verifies the QTI component, PRIME transport, and successful 120/120 token.
  AV1 remains hidden because standard VA-API
  does not supply enough original bitstream syntax for exact reconstruction.

“Zero CPU raw-pixel copies” is not “zero work” and is not a promise that a
codec performs no internal conversion. The one-GPU-blit path must always be
described as such.

## 2026-08-28 long-run and bottleneck audit

The isolated reference-device matrix was extended before the v3 packaging
cut. H.264 Main with B-frames completed 180/180 frames at 147.28 fps, HEVC
Main with B-frames completed 120/120 at 118.73 fps, and VP9 Profile 0 completed
120/120 at 108.46 fps. Every case used QCOM modifier
`0x0500000000000001` as decoder output and an explicit modifier-0 LINEAR
destination, with one Vulkan image repack, zero CPU raw-pixel copies, EOS, and
clean codec stop. Average repack time was 1.379, 1.619, and 1.738 ms,
respectively.

H.264 Main+B produced no output without its bounded reorder contract. The
isolated long-run matrix originally used a four-frame reconstruction bound,
but displayed Firefox ESR/RDD acceptance exposed a smaller submission window:
four and two withheld the first export, while zero damaged ordering. A bound
of one was the only candidate that completed playback and repeated seeks.
The packaged session therefore exports exactly
`ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1` after the complete
codec preflight. H.264 High, HEVC Main10, and AV1 remain unadvertised. HEVC
Main and VP9 Profile 0 are now advertised as public beta after their independent
live preflights; displayed playback and seek remain application-specific
compatibility evidence.

The packaged v3.0.3-dev ZIP was installed through KernelSU and exercised
through its ordinary production gateway, without the experimental Android
low-latency codec switch. Firefox kept RDD seccomp mode 2, held zero KGSL file
descriptors, completed 30 seeks and 10 document reloads, and reported 766
decoded frames with 5 dropped. The VA driver recorded 1,757 hardware outputs,
1,758 successful LINEAR exports, zero export timeouts, and zero asynchronous
failures; Android media metrics named `c2.qti.avc.decoder` and reported low-
latency mode off. Stopping the active session left no Firefox, gateway, Sway,
or session-runner orphan, and the next Sway session started normally. The
public release ZIP still needs the exact tagged-release repetition and a
per-frame gateway fence-count audit.

The corresponding 720p60 encode audit completed H.264 Constrained Baseline
120/120 at 84.9 fps and HEVC Main 120/120 at 81.1 fps on the QTI hardware
encoders. Generic FFmpeg upload accounted for one CPU raw-pixel copy and one
GPU conversion per frame; the PRIME/OBS route accounted for zero CPU raw-pixel
copies and the same one GPU conversion. VP8, VP9, and AV1 hardware encoders
were absent and remain hidden. Encode input is explicit LINEAR NV12; compressed
QCOM input is not accepted or relabeled.

## VA-API encode milestone

An isolated reference-device run on 2026-08-26 completed real FFmpeg
`h264_vaapi` and `hevc_vaapi` sessions through the ADVC driver and QTI
MediaCodec components. The exact module later repeated the bounded application
matrix described below:

- `vaSyncBuffer` is now implemented and registered. The FFmpeg capability probe
  `vaSyncBuffer(display, VA_INVALID_ID, 0)` returns
  `VA_STATUS_ERROR_INVALID_BUFFER`, so FFmpeg selects its asynchronous coded-
  buffer path instead of falling back to `vaSyncSurface` after every frame.
- The Vulkan Surface producer now gives `VK_GOOGLE_display_timing` an absolute
  monotonic display time. A media PTS such as 0 or 33 ms is not a valid
  `desiredPresentTime`; using it caused QTI Surface encoding to emit the first
  IDR and then stall. Logical media PTS values remain in the broker's ordered
  frame-token mapping and are restored on coded output.
- H.264 Constrained Baseline and HEVC Main both passed 5 frames at FFmpeg
  `async_depth` 2, 3, and 4. At depth 3 both completed 30/30 frames, including
  the final pending tail (`2 -> 1 -> 0`), and software decode recovered all 30
  frames. Android media metrics independently recorded the matching hardware
  sessions on `c2.qti.avc.encoder` and `c2.qti.hevc.encoder`.
- Fresh-registry GStreamer runs completed both codecs. H.264 Constrained
  Baseline passed 5/5 frames (21,982 bytes, EOS in 68 ms) and 30/30 frames
  (126,978 bytes, EOS in 644 ms). HEVC Main passed 5/5 frames (20,717 bytes,
  EOS in 72 ms) and 30/30 frames (140,132 bytes, EOS in 710 ms). `ffprobe` and
  software decode recovered every frame. ADVC userspace call logs showed
  successful `vaSyncSurface`, coded-buffer map/unmap, and `DestroyContext`
  return. Android media metrics independently recorded the 30-frame QTI AVC
  and HEVC hardware sessions.
- Those ordinary GStreamer/FFmpeg application runs used the separately gated
  generic NV12 upload route: the CPU-origin image is written into a LINEAR
  dma-buf and hardware encoding proceeds from there. They prove application
  hardware-codec operation, but they are not a zero-copy input claim. Only an
  explicitly imported PRIME/texture path may carry that claim.
- OBS completed both bounded discovery and an actual H.264 texture-path
  recording in an isolated headless Wayland session. It exposed
  `ffmpeg_vaapi_tex` for H.264 and `hevc_ffmpeg_vaapi_tex` for HEVC, rejected
  AV1, and repeatedly exported the same writable VA surface with independent
  PRIME file descriptors. The recording crossed the former accidental
  120-frame runtime limit and stopped cleanly at 363 output frames. `ffprobe`
  reported H.264 Constrained Baseline, 320x240, `yuv420p`, 30 fps, and 12.10 s;
  software decode recovered all 363 frames. The OBS log recorded
  `Recording Stop`, `DestroyContext` returned success, and no generic-upload
  fallback, export failure, or encode error occurred.
- The exact packaged module repeated the same route through the production
  broker after KernelSU activation: 364/364 decoded frames over 12.133 seconds,
  369 successful writable exports, clean `StopRecord`/`DestroyContext`, no
  export/fallback/EndPicture/encode error, and no OBS, Sway, or DBus orphan.
- The installed module also recorded HEVC Main through
  `hevc_ffmpeg_vaapi_tex`: 364/364 decoded frames over 12.133 seconds and 369
  successful writable exports, with the same clean-stop and no-fallback
  conditions. An explicit OBS H.264 VBR profile completed 362/362 decoded
  frames and 367 writable exports. Android media metrics identified the exact
  hardware components as `c2.qti.hevc.encoder` profile 1 and
  `c2.qti.avc.encoder` profile 65536, both with `bitrate_mode=VBR`.
- Encoder advertisement is intentionally narrower than the device-wide codec
  list. ADVC exposes H.264 Constrained Baseline and HEVC Main only; OBS rejected
  H.264 Main/High, HEVC Main10, and AV1, while FFmpeg enumerated only the two
  usable VA-API encoders. Protocol v1 accepts VBR only, and the Android backend
  now sets MediaCodec `bitrate-mode=VBR` explicitly. An exact diagnostic CBR
  override remains test-only and is not advertised.
- GStreamer discovery must fail closed unless the chroot exposes
  `/proc/self/fd`, `/sys/class/drm`, and `/dev/dri/renderD128`, and explicitly
  selects that render node with `GST_VAAPI_DRM_DEVICE`. Without `/proc` and
  `/sys`, libdrm cannot classify the FD as a render node and libva never reaches
  the ADVC driver. On this Android/chroot combination the forked plugin scanner
  dropped the large VA-API feature reply and blacklisted `libgstvaapi.so`.
  `GST_REGISTRY_FORK=no` keeps discovery in process; together with
  `GST_VAAPI_ALL_DRIVERS=1`, it exposed `vaapih264enc` and `vaapih265enc` plus
  their VBR rate-control property. These variables are exported only inside the
  same successful fail-closed ADVC runtime gate.
- The focused imported dma-buf encode ingress had zero CPU raw-pixel copies and
  one Vulkan GPU conversion/blit per frame into the MediaCodec input Surface.
  This statement excludes the generic CPU-origin upload runs above. It is not
  a direct scanout or zero-GPU-work path.

### Encode input copy accounting

The two application input routes have deliberately different accounting:

- `vaPutImage` generic upload performs one logical CPU raw-pixel copy for each
  successful accepted upload. NV12 uses plane-row copies; I420 additionally
  interleaves U/V into NV12. The implementation now exposes a monotonic copy
  counter, and tests verify that successful uploads increment it exactly once
  while rejected uploads do not.
- Writable DRM PRIME export duplicates descriptor FDs but does not copy pixel
  bytes. Repeated exports of the same VA surface are counted separately because
  OBS legitimately asks for more than one owned descriptor before
  `EndPicture`. `EndPicture` snapshots the final implicit producer fence and
  submits that surface to one broker GPU conversion/blit.
- A frame that used both routes is reported as `mixed`; it must never be
  described as zero CPU-copy. Trace accounting reports CPU copies, writable
  exports, and successful GPU conversion submissions separately. There is no
  silent fallback from PRIME write-export to generic upload inside the driver.

The normal Vulkan behavior is the absolute monotonic schedule. The old logical-
PTS-as-display-time behavior is available only behind the exact diagnostic
gate `ADVC_VULKAN_LEGACY_PRESENT_PTS=diagnostic-only-v1`; it must not be used in
release sessions. About one minute after the successful GStreamer H.264 run,
the device rebooted while low-level ftrace collection was still active. A later
`last_kmsg` record identified a shell-requested reboot and a `Bad page state`
BUG in the temporary-root exploit process (`cve43499-rootho`), providing kernel
memory-corruption evidence in that root-recovery path rather than the completed
codec session. The codec had already reached EOS and destroyed its context.
That tracing method is retired regardless. The actual OBS encode passes both on
the isolated exact driver/broker pair and on the installed release module. The
host release verifier pins those exact payloads. Runtime advertisement still
fails closed unless all required mounts, device nodes, socket identity, and
manifest digests pass.

## VA-API decode candidate milestone

The 2026-08-26 isolated reference-device decode milestone initially used broker
`678d72f9…df4211` and a 200,488-byte driver
`2dca711a…492e4`. The descriptor-ownership fix was then rebuilt reproducibly as
the 200,600-byte driver `bd8c8a05…f5e70` and installed in the 2026-08-27
release snapshot. The transport results below were repeated with that exact
driver; the old 200,488-byte file is historical evidence only.

- Direct QCOM UBWC completed 120/120 nonempty H.264 Main frames plus EOS. The
  broker returned 121 outputs including EOS, 120 AHB/PRIME exports, 120
  acquire/release-fence round trips, and 121 release calls. Turnip validated
  content and crop on every exported frame. The exact modifier was
  `0x0500000000000001`.
- The gated LINEAR route completed the same 120/120 frames plus EOS. All 120
  exported descriptors carried explicit `modifier = 0`; the Vulkan repack
  returned a destination acquire fence and a source release fence for every
  frame. The LINEAR content hash matched the direct UBWC validation hash.
  Implementation and trace accounting show one Vulkan image-to-image repack
  and zero CPU raw-pixel copies per frame.
- Asking the reference MediaCodec/AImageReader path for an original LINEAR
  allocation still produced the QCOM UBWC modifier. LinDeX therefore never
  relabels that allocation as LINEAR. The explicit LINEAR path on this device
  is the GPU-repacked destination, not an original decoder buffer.
- The runtime gate is an exact match:
  `ADVC_VAAPI_GPU_LINEAR_REPACK=validated-qcom-nv12-v1`. With no gate, a wrong
  value, a LINEAR source, or malformed QCOM metadata, the hook fails closed.
  `ADVC_VAAPI_DECODE_OUTPUT=qcom` also requires
  `ADVC_VAAPI_QCOM_IMPORT=validated-v1`; an output selector cannot bypass the
  validated modifier policy. Deprecated `ADVC_VAAPI_OUTPUT` is consulted only
  when the direction-specific selector is unset.

Application evidence is deliberately separated from the transport probes:

- Earlier `vainfo` runs enumerated H.264 Constrained Baseline and Main VLD only. The broker
  reports QTI HEVC, VP9, and AV1 decoder components. Isolated HEVC Main I/P/B
  and VP9 Profile 0 key/inter now pass VA-API transport, QCOM PRIME, release,
  EOS and codec stop. The validation-only HEVC gate also mapped decode order
  `0,1,3,2` to POC and byte-exactly matched four Turnip GPU-read NV12 crops
  against software decode after POC sorting. The stock session now advertises
  HEVC Main and VP9 Profile 0 as public beta only after each exact live
  preflight passes. Displayed application lifecycles and seek remain open
  compatibility tests rather than caller-supplied enable tokens. AV1 stays hidden for a
  stronger reason: FFmpeg's standard AV1 VA-API path supplies
  header-stripped tile data rather than the original OBU stream, and the
  public VA parameters omit syntax needed for an exact OBU reconstruction.
  Guessing those bits would create a false hardware-decode contract.
- FFmpeg selected VA-API hardware decode and completed 30 H.264 Main frames on
  the final isolated candidate. This was a VA surface path; the null output did
  not force all 30 surfaces through DRM PRIME export, so it is not used as the
  PRIME lifecycle proof.
- GStreamer enumerated `vaapidecodebin`, `vaapih264dec`, and `vaapisink` with a
  fresh registry. The explicit GStreamer 1.26 old-VAAPI H.264
  `memory:VASurface ! fakesink` pipeline decoded all four frames, posted
  downstream and bus EOS, exited within the bound, discarded zero codec
  outputs and returned codec stop status 0 with destroy drain disabled. The
  previous timeout was caused by `advc_dmabuf_descriptor_close()` closing an
  undeclared zero slot and therefore GStreamer's FD 0 bus/poll wakeup handle.
  Restricting cleanup to the unique objects declared by `object_count` fixes
  the lifecycle and is covered by a host regression test. This narrow pass did
  not invoke PRIME import, a video sink, playbin/decodebin, seek or display, so
  it is not yet a general GStreamer application-decode claim.
- mpv `--hwdec=vaapi-copy` selected the H.264 VA-API decoder and then failed at
  the intentionally unimplemented `vaGetImage` CPU-download path. A true GPU
  zero-copy mpv VO was not tested because this run did not use DP output.

The 120-frame smoke/content executables and fixtures are development evidence
only. The HEVC EOS/POC hook and NV12 hash helper are also validation-only. None
of these executables, hooks, samples, or fixtures may be added to a release
module; the release verifier rejects them by name and path.

## Module application advertisement

The module now packages the ARM64 libva vendor driver at
`payload/debian/codec/advc_drv_video.so`, verifies it in the module artifact
manifest, and installs it immutably at
`/opt/android-drm-lease-kit/codec/vaapi/advc_drv_video.so` inside the chroot.
The release verifier requires an ELF64 AArch64 payload and matching SHA-256
manifests. Smoke and synthetic test executables remain source/development
artifacts and are forbidden from the release ZIP.

The v3.0.3 source-module codec artifacts are:

- broker: 141,480 bytes, SHA-256
  `34d8a0dbfa7c3f15ffb3ddfc3c419c951610b043323caf5db2c4d3bb253f2954`;
- capability probe: 140,352 bytes, SHA-256
  `caa424606c7544ab45e50020b639e6c31eb9e5e960e798abd79303579d76a8be`;
- VA driver: 200,312 bytes, SHA-256
  `5c695313ebd8bf88693b94fb05eaa08b38b147cefad876b4d1035894940d9973`;
- decode preflight: 67,576 bytes, SHA-256
  `dbe634d840bffebdb85afc3f9402fe2754e8b3d1072a13f0ce5e191f227111a9`;
- repack gateway: 133,192 bytes, SHA-256
  `acc31bbd9c31673ebeacfb468d41baf0244271d4610dc462d77b677aae52668f`;
- Firefox RDD socket adapter: 67,344 bytes, SHA-256
  `3b29a65894fac042c5c8f480623569bc8516e3ec27300783466591033ab502e9`.

They passed reproducible ARM64 builds, host video 15/15, VA-API 14/14, module
integration, profile, WebUI, hotplug, and package verification. They are in the
final release/dev ZIP candidates but have not yet been installed together on
the reference device. The installed 2026-08-27 snapshot below is historical
activation evidence:

- broker: 126,768 bytes, SHA-256
  `678d72f908955bf7a1be11976199deaf7bf63b06e56b4d9e42d56fb3c6df4211`;
- driver: 200,600 bytes, SHA-256
  `bd8c8a059cd150f512ae517ba4cc08c49b100ca796a3e624d72581818d1f5e70`.

The older 134,520-byte driver `0bfac348…d064c3` and integration ZIP below are
retained only as historical encode evidence. On the reference device, that
driver passed the loader/vtable gate with three profiles
and two image formats. The encode application matrix above includes the
isolated GStreamer H.264/H.265 and 363-frame OBS runs. The historical
integration ZIP, SHA-256
`56b5d6dcd4db8377b76a6daaba0f8b33fa544379572b6ab7aa78e96ef4fe51e5`,
was installed and activated on the reference device. It repeated FFmpeg
H.264/HEVC encode at 30/30, GStreamer H.264/H.265 encode at 5/5, and OBS at
364/364 software-decoded verification frames while retaining the production
broker PID. That ZIP predates the current query-status, multi-slice, HEVC, and
VP9 decode source changes and is not a release artifact for the current tree.

When `VIDEO_ACCELERATION=auto`, the outer launcher first reconciles the exact
identity-checked production broker. The inner session exports the following
client selectors and exact validation gates only after independently verifying
the driver digest, broker socket, render node, procfs FD view, and DRM sysfs
view:

- `LIBVA_DRIVER_NAME=advc`
- `LIBVA_DRIVERS_PATH=/opt/android-drm-lease-kit/codec`
- `ADVC_VAAPI_SOCKET=/run/android-drm/advc-repack-gateway-<session>.sock`
- `ADVC_VAAPI_DECODE_OUTPUT=auto`
- `ADVC_VAAPI_ENCODE_INPUT=auto`
- `ADVC_VAAPI_ASYNC_EXPORT=candidate-firefox-bframe-v1`
- `GST_VAAPI_DRM_DEVICE=/dev/dri/renderD128`
- `GST_VAAPI_ALL_DRIVERS=1`
- `GST_REGISTRY_FORK=no`
- `ADVC_VAAPI_ENABLE_AVC=validated-v1`
- `ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1`
- `ADVC_VAAPI_ENABLE_ENCODE=validated-avc-hevc-v1`
- `ADVC_VAAPI_ENABLE_GENERIC_UPLOAD=validated-nv12-v1`
- `ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1`

`VIDEO_ACCELERATION=disabled`, an invalid policy, or any missing file, mount,
device, manifest, or socket leaves every selector and validation gate unset.
The generic-upload gate is the normal compatibility route for CPU-origin NV12;
it is not the OBS texture/PRIME zero-copy route. The OBS texture write-export
route passed its isolated live gate with repeated same-surface exports and 363
decoded frames. The exact gate
`ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1` may therefore be
exported only after the packaged driver/broker manifest and runtime preflight
pass; otherwise it remains unset. The desktop may still start with its normal
software-codec fallback; LinDeX does not partially advertise ADVC.

## Transport status

| Direction | Transport | Current status | Release statement |
|---|---|---|---|
| Encode | byte input | Functional compatibility path | Copies input bytes; not zero-copy |
| Encode | Android-local AHB/Surface | Functional development path | Android-local hardware path |
| Encode | Debian dma-buf via Vulkan | Isolated live gate and installed-module OBS H.264/HEVC texture recording passed | Zero CPU raw-pixel copies; one GPU blit; repeat on the public release tag |
| Encode | Debian dma-buf via EGL | Not exposed on the reference stack used for the focused probe | Unsupported unless its own live probe passes |
| Decode | byte output | Reference-device PASS: 720x360, color format 21/NV12, stride 768, slice height 384, 436,176 bytes | One raw-frame CPU copy; repeat on exact release artifact |
| Decode | private AHB output | Reference QTI decoder produced QCOM-compressed output even when YUV_420_888/CPU usage was requested | Android-local output; not a direct LINEAR claim |
| Decode | DRM PRIME LINEAR (`modifier = 0`) | Isolated candidate PASS: 120/120 frames plus EOS through one Vulkan image-to-image repack per frame; destination content matched direct UBWC; zero CPU raw-pixel copies. Original decoder LINEAR remains unavailable on the reference device | The exact driver is pinned in the source module; keep the broader repack gate unset until packaged-player display and seek pass |
| Decode | DRM PRIME QCOM UBWC | Isolated candidate PASS: 120/120 frames plus EOS, content/crop, acquire/release fences, repeated release and teardown; NV12 `0x3231564e`, modifier `0x0500000000000001`, one object/two planes, no raw-pixel CPU copy | H.264 QCOM import may be advertised only behind the exact gate and complete module preflight |

Strict zero-copy mode permits only a backend that passed its live probe. It
never silently changes to byte transport or a CPU raw-pixel copy. Auto mode may
choose a documented fallback at session creation; it does not switch mid-frame.

## Descriptor and modifier rules

A PRIME descriptor is authoritative only when it supplies all required
metadata: DRM fourcc, explicit modifier semantics, allocation/object count,
plane-to-object mapping, offsets, strides, logical crop, and owned file
descriptors/fences.

Modifier value `0` has a special but unambiguous rule:

- `0` is valid **DRM LINEAR** only when the descriptor's explicit-modifier flag
  is set;
- `0` without that explicit declaration is missing/ambiguous metadata and must
  be rejected;
- QCOM UBWC must carry its exact nonzero modifier and validated auxiliary/data
  layout; it must never be inferred from device family or buffer dimensions.

The decoder and importer must validate the `modifier = 0` LINEAR case as
strictly as a nonzero modifier: plane bounds, object sizes, format, crop,
acquire fence, content, and release fence all remain mandatory. The explicit
LINEAR contract is covered at unit level. On the reference Samsung/QTI device,
requesting YUV_420_888 plus CPU usage still returned QCOM-compressed output, so
it did not prove a direct original-buffer `modifier = 0` path. The public matrix
therefore distinguishes that unavailable direct allocation from the validated
one-Vulkan-repack LINEAR destination.

## Decode PRIME advertisement boundary

A decoded private AHardwareBuffer can contain vendor-specific plane and
compression metadata. Exporting file descriptors is not enough to prove that a
Debian consumer will interpret the image correctly. The 120-frame QCOM UBWC and
GPU-repacked LINEAR results prove two narrow H.264 routes. They do not justify
a general decoder capability. Advertisement must remain bound to:

1. stable mapper metadata decoding without a private vendor C++ ABI;
2. exact image-object selection and owned PRIME FD transfer;
3. the exact QCOM gate, or the exact LINEAR repack gate plus its Vulkan runtime;
4. GPU content validation using the logical crop;
5. acquire-fence wait and a real release-fence round trip when asynchronous
   consumer work occurs;
6. repeated release, malformed-descriptor rejection, disconnect cleanup, and
   session recreation; and
7. no capability advertisement when any required check fails.

The installed 200,600-byte driver satisfies the bounded H.264 QCOM transport
lifecycle and contains the validated LINEAR repack implementation. The stock
session still leaves the broader LINEAR gate unset until exact packaged-player
display and seek pass. Byte, VA-surface-only, and Android-local decode evidence
must never be relabeled as DRM PRIME validation. HEVC has passed its isolated
pixel gate but remains hidden until the application gate passes. VP9 remains
hidden pending its own pixel/application evidence; AV1 remains intentionally
unsupported.

## Release packaging boundary

Release modules contain only the reviewed runtime broker/service, ARM64 VA-API
driver, and the bounded capability probe required for live gating. Other diagnostic and
synthetic executables and their streams/logs are development artifacts and must
not be included in a published module ZIP.

Source tests may remain in the repository for reproducibility. Their presence
in source does not authorize packaging them into the module.

## Final codec record template

For each final packaged-module result, add one row to
[Validation status](VALIDATION_STATUS.md) with:

- Git commit, module ZIP SHA-256, build flavor, device model, Android version;
- codec component and input/output transport;
- frame count, nonempty outputs, total bytes, EOS, and bounded runtime;
- CPU raw-pixel copy count and GPU work count;
- format, modifier, object/plane layout, crop, and fence result where relevant;
- repeated-run and teardown result; and
- explicit distinction between packaged-module and isolated-probe evidence.

Do not publish final numbers until this exact record is available.
