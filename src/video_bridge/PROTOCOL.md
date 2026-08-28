# LinDeX ADVC Protocol 1.8

[English](PROTOCOL.md) | [한국어](PROTOCOL.ko.md)

Project: **LinDeX**  
Author: **JHoon**

The transport is `AF_UNIX/SOCK_SEQPACKET`. One ADVC message occupies one socket
record. Every integer is unsigned little-endian. Receivers reject records larger than
1 MiB and messages containing more than eight file descriptors.

The production filename may remain `advc-broker-1.1.sock` as a stable compatibility
endpoint. That filename is not a protocol-version assertion: every connection still
negotiates its header minor version and feature intersection, including 1.8.

## Header

The 32-byte header contains:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `ADVC` |
| 4 | 2 | major version |
| 6 | 2 | minor version |
| 8 | 2 | request/reply/event |
| 10 | 2 | opcode |
| 12 | 4 | request ID |
| 16 | 4 | session ID |
| 20 | 4 | flags |
| 24 | 4 | payload bytes |
| 28 | 2 | attached FD count |
| 30 | 2 | reserved, must be zero |

Major-version mismatch is fatal. Minor versions may add flag bits or append payload
fields; existing offsets do not change. Discovery operations remain compatible with
1.0, but codec-session operations require a client minor version of at least 1 and
return `UNSUPPORTED` to a 1.0 client. AHardwareBuffer operations require minor
version 2; a 1.1 client remains byte-transport compatible.
Broker-local EGL encoder input requires minor version 3; 1.2 AHB and byte clients
retain their existing layouts and semantics.
Android-local external AHB encoder input requires minor version 4. Minor 3 clients
retain the synthetic Surface route but cannot observe or select external AHB input.
Version 1.5 adds an explicit Debian dma-buf registration/submission contract.
`DMABUF` is creatable only when the selected backend has real exact-import and
submit callbacks. `FEATURE_DMABUF_EGL` and `FEATURE_DMABUF_VULKAN` are
backend-specific, fail-closed probe results; `FEATURE_DMABUF` is their aggregate
gate. Advertising any of them requires an actual dma-heap import, codec-Surface
submission, frame drain, release sync-file, and EOS.
`FEATURE_DECODE_PRIME` is a separate real-probe gate for MediaCodec PRIVATE AHB
output exported as an authoritative DRM PRIME descriptor. It is off by default
and must remain clear if crop metadata is not per-plane, if more than one
transport FD is present without a public plane-to-object mapping, or until a
Debian consumer completes a real import and release-fence round trip.
Version 1.6 adds `TRANSFER_PRIME`, which transfers authoritative DRM PRIME
metadata and owned dma-buf object FDs for one outstanding decoded AHB output.
The acquire fence remains the one returned by `DEQUEUE_OUTPUT`; transferring
PRIME does not retire the codec output, which still requires `RELEASE_OUTPUT`.
Version 1.8 adds `RESERVE_LINEAR` and `ASYNC_DECODE_PRIME`. A repack gateway can
return a stable empty LINEAR dma-buf before a reordered decoder output exists;
the later output is GPU-repacked into it and its acquire fence becomes the
content-completion point. It also adds modifier-specific decode/encode QCOM
capability bits. Those bits report independently gated transport/import
capabilities, not a request to relabel memory or to select QCOM unconditionally.
For every supported request minor from 0 through the server's current minor, the
reply carries that exact request minor. A request above the current minor receives
`UNSUPPORTED` at the server's current minor, with that minor in status detail.
HELLO and capability replies below minor 3 mask `BROKER_EGL_SURFACE`; creating that
transport still requires request minor 3.
HELLO and capability replies below minor 4 also mask `ANDROID_AHB_SURFACE`.
HELLO and capability replies below minor 5 mask `DMABUF`, `DMABUF_EGL`, and
`DMABUF_VULKAN`. Replies below minor 6 mask `DECODE_PRIME`.
Replies below minor 8 mask `ASYNC_DECODE_PRIME`, `DECODE_QCOM_MODIFIER`, and
`ENCODE_QCOM_MODIFIER`.

## Operations

- `HELLO`: the client sends its feature mask and maximum packet size. The broker
  replies with the intersection. This prevents a glibc client from negotiating the
  Android-only AHardwareBuffer API accidentally.
- `QUERY_CAPABILITIES`: returns selected MIME/component pairs and whether a component
  is known hardware, software, vendor software, or unknown.
- `CREATE_SESSION`: configures and starts a codec. The request has no FD,
  session ID zero, and an exact 96-byte payload containing direction, width, height,
  bitrate, frame rate in milli-Hz, flags, a NUL-terminated MIME string, and an Android
  color format at offset 88 and an exact transport at offset 92. Zero retains legacy
  byte transport. AHardwareBuffer and broker-local EGL Surface input must be
  requested explicitly and are never an
  implicit fallback. Decode requires color format zero. The bounded encode
  subset accepts only `video/avc` and `video/hevc`, even dimensions, a nonzero bitrate
  no greater than 200 Mbit/s, 1--240 fps, and either Android
  `COLOR_FormatYUV420Planar` (19, tightly packed I420) or
  `COLOR_FormatYUV420SemiPlanar` (21, tightly packed NV12 with UV chroma order).
  Version 1.3 also accepts transport `BROKER_EGL_SURFACE` with color format zero.
  It configures `COLOR_FormatSurface`; the client never supplies raw pixels.
  Version 1.4 accepts `ANDROID_AHB_SURFACE` with color format zero. It uses the
  codec Surface but accepts pixels only through the separate AHB handshake below.
  Create flags must be zero. A successful status
  reply carries the allocated nonzero session ID in its header.
- `QUEUE_INPUT`: submits one compressed decoder access unit/codec-configuration
  buffer or one tightly packed encoder frame.
  PTS is nanoseconds and is converted to MediaCodec microseconds. `END_OF_STREAM`,
  `KEY_FRAME`, and `CODEC_CONFIG` are the only accepted flags. Input is either inline
  immediately after the 48-byte fixed payload, or in exactly one regular memfd with
  role `INPUT_DATA`. An input memfd must carry shrink, grow, and write seals; offset
  and size must be within the file. Encoder frames must have flags zero and exactly
  `width * height * 3 / 2` bytes. Encoder EOS must be a separate empty buffer carrying
  only `END_OF_STREAM`; client-supplied encoder `KEY_FRAME` and `CODEC_CONFIG` are
  rejected.
  For `BROKER_EGL_SURFACE`, each ordinary frame is instead an exact empty inline
  packet with flags zero and PTS: its payload is exactly 48 bytes, data offset is
  48, data size is zero, FD role is `NONE`, and no FD is attached. Even a sealed
  zero-length memfd is rejected. The broker renders one deterministic solid-color
  GLES frame into the codec ANativeWindow, applies that PTS with
  `eglPresentationTimeANDROID`, and swaps once. Production sessions have no
  fixed frame-count limit; the internal frame sequence is an unsigned 64-bit
  counter. Smoke tools keep their own bounded frame limits. Frame sequence `i`
  has RGBA `(37i+17, 67i+53, 97i+101, 255) mod 256` (alpha is always 255).
  At most one rendered frame may await compressed frame output; further
  ordinary queues return `WOULD_BLOCK` until the client dequeues nonempty encoded
  non-codec-configuration output, bounding codec-Surface backpressure. The first
  synchronous `eglSwapBuffers` remains a vendor call without a public timeout;
  feature probing and the one-frame window keep that residual risk bounded in scope.
  EOS is the same separate empty EOS control packet and calls
  `AMediaCodec_signalEndOfInputStream` exactly once.
- `DEQUEUE_OUTPUT`: is a nonblocking request with no payload or FD. `WOULD_BLOCK`
  means that no decoded output is ready or that an output-format change was consumed.
  Success returns one sealed memfd containing a copy of the MediaCodec byte buffer and
  a 136-byte version 1.1 metadata payload. The common fields include output ID, PTS,
  size, flags, byte transport, dimensions, Android color format, and stride. The
  appended fields contain slice height and crop rectangle. DRM fourcc/modifier and
  plane metadata remain zero because this is not a dma-buf or zero-copy path. Safe
  byte-output dequeue requires Android API 36 or later: only then does the public NDK
  guarantee valid output offset and buffer capacity. Older runtimes return
  `UNSUPPORTED` after releasing the codec buffer rather than risking an out-of-bounds
  copy. For encode, the memfd contains compressed AVC/HEVC bytes and MediaCodec output
  flags are mapped to ADVC `CODEC_CONFIG`, `KEY_FRAME`, and `END_OF_STREAM`. An output
  format-change notification is consumed nonblockingly and returned as `WOULD_BLOCK`;
  codec configuration is delivered only when MediaCodec emits its corresponding
  output buffer. Compressed output has no raw Android color/stride/plane claim.
  An AHardwareBuffer decode session instead configures MediaCodec with an
  `AImageReader` PRIVATE surface. A decoded image returns the 112-byte common
  metadata payload, transport `AHARDWAREBUFFER`, and zero byte size. Width, height,
  layers, Android format, stride, and usage come from `AHardwareBuffer_describe`.
  DRM fourcc, modifier, and planes remain zero: the broker never guesses Qualcomm
  layout. An optional acquire-fence FD has role `ACQUIRE_FENCE`.
  A zero-sized MediaCodec Surface EOS has no AImage. The broker releases that codec
  buffer with rendering disabled and returns one empty sealed byte-transport output
  carrying EOS and the current layout. This is a control record, copies no pixels,
  never waits on AImageReader, and makes later dequeue calls return `WOULD_BLOCK`
  until flush.
- `TRANSFER_AHB`: has an exact eight-byte output ID payload. It is valid once for an
  outstanding AHardwareBuffer output. The successful status reply carries
  `AHB_FOLLOWS`; immediately after that record the broker sends the native handle.
  Repeated transfer, byte-output IDs, and unknown IDs are rejected.
- `TRANSFER_PRIME`: is the version 1.6 decoded-AHB export operation. It has an
  exact eight-byte output ID payload and no attached FD. It is valid once for an
  outstanding AHardwareBuffer decode output, only when `DECODE_PRIME` was
  negotiated and the backend provides the gated export callback. Success returns
  an eight-byte status followed by the exact 256-byte dma-buf descriptor used by
  `REGISTER_DMABUF`, plus exactly one owned `CLOEXEC` FD for each declared object.
  The descriptor buffer ID must match the requested output ID. The operation does
  not consume the AHB or its acquire fence and does not retire the codec output;
  the client closes the returned object FDs and still sends `RELEASE_OUTPUT` with
  any release fence. Repeated transfer, zero/unknown IDs, malformed descriptors,
  FD-count mismatch, and unavailable export capability are rejected fail-closed.
- `RESERVE_LINEAR`: is the version 1.8 gateway-local operation. It carries an
  exact PTS, visible width/height, and NV12 fourcc, with no FD. Success returns
  an owned, modifier-0 destination descriptor immediately. The eventual
  `DEQUEUE_OUTPUT` for the same microsecond-normalized PTS binds decoded content
  and an acquire fence to that allocation. Reservation is unavailable in forced
  QCOM pass-through mode and never preallocates a compressed QCOM image.
- `QUEUE_AHB` / `COMPLETE_AHB`: form one bounded version 1.4 input handshake for an
  `ANDROID_AHB_SURFACE` encoder session. `QUEUE_AHB` carries exact PTS, width,
  height, Android format, layers, usage, flags zero, and optional acquire-fence role.
  It attaches either no FD/`FD_NONE` or exactly one `ACQUIRE_FENCE` FD. An OK reply
  with `AHB_FOLLOWS` permits the Android client to send exactly one public
  `AHardwareBuffer_sendHandleToUnixSocket` record. The broker receives it with
  `AHardwareBuffer_recvHandleFromUnixSocket`, compares its public descriptor exactly
  with the request, and never parses native-handle integer slots. `COMPLETE_AHB`
  returns render status and either no FD/`FD_NONE` or one `RELEASE_FENCE` FD. Only
  one handshake may exist per session. The broker consumes its acquire-fence
  duplicate on every return; the client owns the returned release fence. A missing
  or malformed native-handle record is connection-fatal, as is a malformed
  completion reply observed by the public client; neither side attempts to resync
  the mixed protocol/native-handle stream.
- `REGISTER_DMABUF`, `UNREGISTER_DMABUF`, `QUEUE_DMABUF`, and `COMPLETE_DMABUF`
  define the version 1.5 Debian ingress path. They are dispatched only for a
  `DMABUF` encoder session whose backend supplies both exact-import validation and
  submission callbacks. Registration is an
  exact 256-byte record with a nonzero client buffer ID, dimensions and crop, DRM
  fourcc, one explicit image modifier, color primaries/transfer/matrix/range/chroma
  siting, one to four objects, and one to four planes. Each object names one attached
  `CLOEXEC` FD by a unique index and an authoritative bounded allocation size. Each
  plane names an object, 64-bit offset, and nonzero pitch. Explicit fourcc, modifier,
  and plane flags are all mandatory; modifier zero means LINEAR only because it is
  explicit, while `UINT64_MAX` is rejected. Registration validation never interprets
  private Android handle slots and never claims that `fstat` proves a dma-buf.
  Before registration succeeds, the selected Android Vulkan or EGL implementation
  must import and destroy the exact descriptor; a guessed format, omitted modifier,
  or structural check alone is insufficient.
  `QUEUE_DMABUF` carries a registered buffer ID, PTS, and either no fence/`FD_NONE`
  or one `ACQUIRE_FENCE` sync-fd. `COMPLETE_DMABUF` identifies the same buffer and
  may return one `RELEASE_FENCE` only on success. `UNREGISTER_DMABUF` is forbidden
  while that buffer is in flight. The Android backend imports the acquire sync_file,
  samples the imported image exactly once into the MediaCodec input Surface, and
  returns a release sync_file ordered after the final source read. It maps, reads
  back, and copies zero raw pixel bytes on the CPU; because the codec Surface owns a
  different allocation, the accurate claim is **zero CPU pixel copies plus one
  bounded GPU blit/draw**, not literal same-allocation pass-through.
- `RELEASE_OUTPUT`: has a 16-byte payload containing the logical output ID and fence
  role. Byte transport requires `FD_NONE` and no attached FD. The MediaCodec output
  buffer was already returned immediately after the broker copied it; release removes
  the logical outstanding-output record and applies client backpressure.
  AHardwareBuffer output accepts zero FDs with `FD_NONE`, or exactly one
  `RELEASE_FENCE` FD. The engine duplicates that FD before the request record is
  released, and the backend consumes it with `AImage_deleteAsync`.
- `FLUSH`: has no payload or FD, flushes MediaCodec, and invalidates all outstanding
  logical output IDs for the session. Broker-local Surface encode returns
  `UNSUPPORTED` because `signalEndOfInputStream` is one-shot; close and recreate the
  session instead.
- `CLOSE_SESSION`: has no payload or FD and stops/deletes the codec.
- `PING` has conventional request/reply semantics.
- Each dma-buf backend is advertised only when its independent, bounded device
  probe allocates a real dma-heap buffer and completes exact import, one GPU
  submission, a valid release sync-file, MediaCodec Surface encode, nonempty
  compressed output, and EOS. Every retry uses a completely new codec Surface
  session. Failure leaves that backend bit clear.

### Debian dma-buf Surface encode (version 1.5 candidate)

The initial importer allowlist is deliberately narrow: one-plane explicit-LINEAR
`ABGR8888` (`AB24`) or `XBGR8888` (`XB24`), exact session-sized crop, RGB/full or
unspecified color metadata, and a complete in-bounds object/offset/pitch tuple.
The default backend order is Vulkan then EGL. Vulkan requires
`VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
`VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, Android Surface and
swapchain support. EGL requires `EGL_EXT_image_dma_buf_import`,
`EGL_EXT_image_dma_buf_import_modifiers`, `EGL_KHR_image_base`,
`EGL_ANDROID_native_fence_sync`, and `EGL_KHR_wait_sync`; GLES must expose
`GL_OES_EGL_image_external`. Every supplied acquire/release fence is verified with
the Linux `SYNC_IOC_FILE_INFO` UAPI, including on Android NDK sysroots that omit the
header. Arbitrary `CLOEXEC` descriptors are not accepted as fences.

Registration retains duplicates of at most four object FDs, up to 16 registered
buffers and four in-flight buffers per session. Completion retires the in-flight
state and transfers ownership of the optional release fence to the client. Session
close deterministically closes registered objects and pending completion fences.
`FLUSH` is unsupported for this one-shot codec-Surface route; close and recreate the
session. The isolated Vulkan route has passed the bounded device probe for the
narrow LINEAR RGB allowlist. EGL remained fail-closed on the tested device because
the required dma-buf import extensions were absent. This is not an installed
production-broker claim.

### Broker-local EGL Surface encode

The feature bit is advertised only after a cached bounded startup probe has actually
configured a 320x240 AVC MediaCodec encoder at 500 kbps and 30 fps, created its input ANativeWindow and a
recordable GLES2 EGLSurface/context, rendered and swapped one frame with an Android
presentation timestamp, signaled Surface EOS, drained a bounds-checked nonempty
compressed output buffer and EOS, and cleaned up. A later codec-specific
session can still fail normally if that component rejects its requested dimensions
or rate. Runtime API below 36 fails this probe before codec creation, matching the
public NDK byte-output safety gate.

This route avoids a CPU raw-frame copy because pixels originate from broker-local
GLES rendering and go directly to the codec Surface. Compressed encoder output still
uses the existing sealed byte memfd. It is a bounded diagnostic/runtime producer,
not Debian application zero-copy, not external AHB import, and not proof of a
particular GPU-to-codec internal layout. `ANativeWindow_lock` is never used.
Each render rebinds its own context and Surface with `eglMakeCurrent`. Producers
share a refcounted EGL display; destroying one producer cannot terminate the display
while a sibling producer remains alive.

### Android AHardwareBuffer Surface encode

The version 1.4 route accepts only public AHB descriptors with one layer, exact
session dimensions, `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM`, and
`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`. Only the sampled-image and CPU-write
usage bits used by the bounded Android test producer are accepted; protected or
unknown usage is rejected. The described stride must be nonzero and at least the
width. EGL must expose
`EGL_ANDROID_get_native_client_buffer`, `EGL_KHR_image_base`,
`EGL_ANDROID_image_native_buffer`, `EGL_ANDROID_native_fence_sync`, and
`EGL_KHR_wait_sync`; GLES must expose
`GL_OES_EGL_image_external`. The broker imports the acquire fence into the GPU
queue, binds the AHB as an external EGLImage texture, draws it to the codec Surface,
applies the exact PTS, swaps once, and exports a native release fence after that
work. This bounded v1.4 implementation also quiesces the submitted GPU read before
returning, so a missing `COMPLETE_AHB`, disconnect, or later protocol error cannot
leave client buffer reuse dependent on an unobservable fence. Runtime submission
maps no pixels on the CPU, though it may perform a GPU blit
or color conversion and is not direct scanout.

The feature bit is advertised only after both a bounded local full-output probe and
a real `SOCK_SEQPACKET` v1.4 create/`QUEUE_AHB`/public-handle/`COMPLETE_AHB`/close
probe succeed against the production backend. Native-handle receipt has a two-second
bound and any missing or malformed handle is a fatal connection error. The probe
maps its own synthetic buffer only to seed known pixels.

This is not a Debian/glibc PRIME path. Public AHB handle send/receive functions are
Android/Bionic APIs, and the handle record remains opaque to glibc. Debian clients
still require a separate explicit dma-buf fourcc/modifier/plane contract and EGL
dma-buf importer; no private handle slots are guessed here.

The public constants in `include/advc/protocol.h` are authoritative for fixed payload
offsets. No C structure is sent directly.

## Buffer and fence ownership

- The sender retains ownership of FDs passed with `SCM_RIGHTS`; the receiver owns the
  received duplicates.
- Version 1.1 byte output uses a broker-created memfd sealed against shrink, grow,
  write, and seal changes. The receiver owns the received FD and closes it normally.
- A byte-mode codec output is copied before `AMediaCodec_releaseOutputBuffer`; no
  MediaCodec or AHardwareBuffer reference crosses the socket.
- In AHardwareBuffer mode the broker retains the `AImage` until `RELEASE_OUTPUT`,
  flush, close, disconnect, or transport failure. The receiver owns its received
  AHardwareBuffer reference and acquire-fence FD; the backend consumes the duplicated
  release fence. This is a real no-CPU-copy decode transport for an Android/Bionic
  importer, not a dma-buf, DRM PRIME, or UBWC claim.
- A successful version 1.6 `TRANSFER_PRIME` gives the receiver ownership of the
  returned dma-buf object FD duplicates. It does not transfer ownership of the
  outstanding AHB or the dequeue acquire fence and does not release the codec
  output. The client closes the PRIME descriptor separately and completes the
  output lifetime with `RELEASE_OUTPUT`.

### Debian/glibc opaque-handle boundary

`advc_glibc_receive_opaque_ahb` is a bounded receiver callback for
`advc_client_transfer_ahb`. It consumes one native-handle socket record, rejects
payload or control truncation, accepts at most eight `SCM_RIGHTS` FDs, requires at
least one FD, verifies `CLOEXEC`, and owns all accepted descriptors until
`advc_glibc_opaque_ahb_close`. Failure closes every visible received FD, including
the bounded prefix of a control-truncated record. The matching metadata validator
also requires the exact ADVC 1.2 full-allocation AHB crop, real descriptor fields,
and a valid optional acquire fence.

The received payload remains deliberately opaque. Android exposes public send and
receive functions for AHardwareBuffer handles, but it does not specify their
native-handle integer layout as a glibc ABI. Those integer slots therefore cannot
be used to infer which FDs are dma-bufs, plane ordering, DRM fourcc, modifier, or
UBWC state. `advc_glibc_ahb_to_prime` always returns `ENOTSUP` after validating the
AHB record and clears its output contract.

The separate `advc_drm_prime_import` structure is the fail-closed AHB-conversion
contract. It accepts modifier zero as LINEAR only when fourcc, modifier, and planes
are all explicitly marked authoritative; it rejects missing metadata, the invalid
modifier sentinel, unused nonempty planes, bad FDs, zero pitches, and non-CLOEXEC
fences. Contract validation does not perform a DRM import ioctl or prove that an FD
is a dma-buf. A future broker-side Android mapper must provide authoritative plane
metadata, and the consuming EGL/Vulkan/libdrm importer must still perform the real
import. ADVC 1.2 does neither, so it cannot truthfully expose glibc zero-copy yet.

Version 1.5 adds a separate client-originated `advc_dmabuf_descriptor` contract. Its
decoder rejects truncated or extended records, nonzero reserved bytes, missing
explicit metadata, invalid counts and FD indices, non-`CLOEXEC` descriptors, zero or
oversized object sizes, crop overflow, unknown color enums, first-row spans outside
an object, and unused nonempty records. The bounded registry requires an external
exact-format policy callback, duplicates object FDs with `CLOEXEC`, rejects duplicate
IDs, retains at most 16 registrations, and permits at most four distinct buffers in
flight. This is source-level validation, not a dma-buf proof, EGL/Vulkan import, UBWC
claim, or feature-advertisement condition.

## Resource and scheduling bounds

- Maximum sessions per connected client: 4.
- Maximum unreleased logical outputs per session: 8.
- Version 1.5 dma-buf foundation: at most 16 registered buffers and four in-flight
  source buffers per registry; one in-flight submission per registered buffer.
- Maximum compressed decoder input, tightly packed raw encoder input, or copied
  output: 16 MiB. Encode session creation is rejected when one complete raw frame
  would exceed that limit.
- Maximum protocol packet: 1 MiB; therefore inline input is additionally limited by
  packet size.
- MediaCodec input dequeue remains nonblocking. Each broker output poll permits one
  bounded 10 ms wait for the Codec2 callback bridge, then consumes at most seven
  immediately available format/buffer-change events without another wait. A request
  therefore cannot become an unbounded codec wait, and clients retain their single
  end-to-end monotonic deadline.
- A client disconnect destroys its per-connection engine and all remaining sessions.

## Android Codec2 output and codec configuration

- A decoder is configured lazily. When its first input is `CODEC_CONFIG`, AVC
  SPS/PPS are placed in `csd-0`/`csd-1` and other codec data is placed in
  `csd-0` before `AMediaCodec_start`. `ADVC_CODEC_CONFIG_AS_DATA` is an explicit
  diagnostic override for the older queued-data behavior.
- An output-format event updates coded size, color, stride, slice height, and crop.
  Encoder `csd-0`/`csd-1` carried only by that event are copied into one bounded
  synthetic `CODEC_CONFIG` output before later compressed buffers are drained.
- Successful input EOS is terminal until flush. The backend maps a zero-length output
  EOS even when MediaCodec exposes no byte-buffer address, and no dequeue is issued
  after output EOS until flush.
- Format-event draining is capped at eight events per request. Additional events and
  ordinary no-output results return `WOULD_BLOCK`; the broker continues serving the
  client's bounded polling protocol.

## Security properties in the foundation

The daemon binds a mode `0660` socket and verifies `SO_PEERCRED.uid == 0`. Packet and
FD counts are bounded, received FDs are `CLOEXEC`, malformed records close attached
FDs, and request IDs are echoed. Production integration should add a dedicated group,
SELinux policy and codec watchdog timeouts. The 1.1 engine additionally uses fixed
session/output limits and rejects unsealed input files, unknown flags, nonzero reserved
fields, invalid session IDs, out-of-range byte regions, oversized buffers, ambiguous
encoder color formats, and incorrectly sized or flagged raw frames.
The 1.5 dma-buf contract additionally rejects implicit format/modifier/plane metadata,
protected or unknown future flags, invalid object/plane mappings, first-row spans
outside an allocation, invalid crop/color fields, missing `CLOEXEC`, duplicate
registrations, busy unregister, and registration/in-flight exhaustion. A real
importer must still apply an exact fourcc/modifier allowlist and treat EGL/Vulkan
import as authoritative.

## Decode smoke client

`advc-decode-smoke` drives one bounded decoder check:

```text
advc-decode-smoke SOCKET MIME WIDTH HEIGHT ACCESS_UNIT
```

`ACCESS_UNIT` must be a regular file no larger than 16 MiB. The tool copies it
to a sealed memfd. For `video/avc`, it requires an Annex-B SPS/PPS-only prefix
followed by an IDR access unit. The prefix is queued first as `CODEC_CONFIG`,
then the remaining IDR range is queued as `KEY_FRAME`; both refer to bounded
ranges of the same sealed memfd. Missing SPS/PPS, malformed NAL boundaries,
non-IDR first VCL, and samples without VCL are rejected before connecting.
Other MIME types retain the single `KEY_FRAME` input behavior. EOS follows the
compressed input, and polling uses one monotonic 10-second deadline for the
whole broker exchange. Every successful byte output is checked for a regular,
sealed memfd whose file size matches the advertised size; its first and last
bytes are read when nonempty, metadata bounds are checked, and the logical
output is released. A successful run requires at least one nonempty output and
an EOS output, then flushes and closes the session. The only stdout record is a
single JSON object suitable for test automation.

The Android build supports `ADVC_SMOKE_AHB=1`. It requires negotiated AHB and native
fence features, receives each handle through the public Android API, waits the
acquire fence within the same 10-second deadline, compares the real descriptor with
the wire metadata, and returns the output. `ahb_outputs` proves only handle,
descriptor, fence, and bounded-lifetime operation; it does not verify pixels.

The access-unit input is not a container demuxer. Success still depends on the
selected MediaCodec decoder's input framing rules. Byte-mode success does not prove
AHB operation. AHB-mode success still does not prove pixel correctness, dma-buf,
UBWC, a glibc importer, encode, or continuous-stream support.

## Encoder device-validation client

`advc-encode-smoke` drives one bounded AVC or HEVC byte-buffer encoder session:

```text
advc-encode-smoke SOCKET MIME WIDTH HEIGHT i420|nv12|surface|ahb FRAMES
```

`FRAMES` is 1--120. For `i420` and `nv12`, the client generates a different
deterministic tightly packed
8-bit frame for every input timestamp, copies each frame into a sealed memfd, drains
output while input is backpressured, queues a separate empty EOS, and closes the
session after EOS. For `surface`, the same client sends empty PTS controls and records producer
`broker-egl-surface`; the broker creates the pixels locally with GLES instead of
allocating raw-frame memfds.
The Android-only `ahb` mode allocates one deterministic RGBA AHB and CPU-fills it
strictly as a test producer. It sends the public handle through the v1.4 socket
handshake, waits the returned release sync-fd before reuse, drains the one-frame
backpressure point, and then queues Surface EOS. Broker import/render performs no
CPU pixel copy, but may perform a GPU blit or color conversion. Success requires
exactly one VCL-bearing output packet per requested input, exact first/last PTS,
monotonic PTS, required parameter sets, a key VCL, and EOS; it does not claim pixel
correctness or Debian PRIME support.
One monotonic deadline covers nonblocking connect, negotiation, capability discovery,
create, every queue/dequeue/release retry, drain, and close.
The default is 20 seconds; `ADVC_SMOKE_TIMEOUT_MS` can select only 1,000--60,000 ms.
An additional hard limit of 2,048 output packets prevents a fast peer from defeating
the time bound.

Before creating the session, the client queries the broker's encoder capability for
the requested MIME and records its selected component name and conservative
acceleration classification. `ADVC_SMOKE_REQUIRE_HARDWARE=1` rejects anything not
classified as hardware. `ADVC_EXPECT_CODEC_NAME` requires an exact capability name
match. The identity is authoritative only for an unoverridden
`createEncoderByType` run. ADVC 1.1 does not return per-session identity, so a broker
using `ADVC_CODEC_NAME` also needs independent broker-log evidence; the client never
pretends the capability result proves that override.

Every nonempty compressed output must parse completely as one of these bounded
forms: Annex-B NAL units, four-byte big-endian length-prefixed NAL units,
AVCDecoderConfigurationRecord, or HEVCDecoderConfigurationRecord. Forbidden NAL
header bits, empty/truncated units, length overruns, malformed parameter-set arrays,
and unparsed trailing bytes are rejected. Success requires AVC SPS+PPS or HEVC
VPS+SPS+PPS, at least one VCL unit, at least one IDR/IRAP unit, and EOS. The JSON
result includes component identity, acceleration, packet/flag counts, syntax form,
NAL and parameter-set counts, VCL timestamp bounds, nonmonotonic PTS observations,
total bytes, and an FNV-1a-64 fingerprint. This is structural validation metadata,
not a substitute for decoding the result with an independent reference decoder.
