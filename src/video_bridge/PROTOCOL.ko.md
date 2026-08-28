# LinDeX ADVC Protocol 1.8

[English](PROTOCOL.md) | [한국어](PROTOCOL.ko.md)

프로젝트: **LinDeX**  
작성자: **JHoon**

전송은 `AF_UNIX/SOCK_SEQPACKET`을 사용합니다. ADVC 메시지 하나는 socket
record 하나를 차지합니다. 모든 정수는 unsigned little-endian입니다. Receiver는
1 MiB보다 큰 record와 파일 디스크립터가 8개를 초과하는 메시지를 거부합니다.

Production 파일 이름은 안정적인 호환 endpoint로 `advc-broker-1.1.sock`을
유지할 수 있습니다. 이 파일 이름이 프로토콜 버전을 주장하는 것은 아닙니다.
모든 연결은 1.8을 포함해 header minor version과 feature 교집합을 협상합니다.

## Header

32-byte header의 구조는 다음과 같습니다.

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
| 30 | 2 | reserved, 반드시 0 |

Major version이 다르면 치명적 오류입니다. Minor version은 flag bit를 추가하거나
payload 뒤에 field를 붙일 수 있지만 기존 offset은 바뀌지 않습니다. Discovery
operation은 1.0과 계속 호환되지만 codec-session operation에는 client minor
version 1 이상이 필요하고 1.0 client에는 `UNSUPPORTED`를 반환합니다.
AHardwareBuffer operation은 minor version 2가 필요하며 1.1 client는 byte
transport 호환성을 유지합니다.

Broker-local EGL encoder input에는 minor version 3이 필요합니다. 1.2 AHB 및
byte client의 기존 layout과 의미는 유지됩니다. Android-local external AHB
encoder input에는 minor version 4가 필요합니다. Minor 3 client는 synthetic
Surface 경로를 유지하지만 external AHB input을 보거나 선택할 수 없습니다.

Version 1.5는 명시적 Debian dma-buf 등록/제출 계약을 추가합니다. 선택된
backend가 실제 exact-import와 submit callback을 모두 가질 때만 `DMABUF`를
생성할 수 있습니다. `FEATURE_DMABUF_EGL`과 `FEATURE_DMABUF_VULKAN`은 backend별
fail-closed probe 결과이며 `FEATURE_DMABUF`는 그 aggregate gate입니다. 어느
bit든 광고하려면 실제 dma-heap import, codec Surface 제출, frame drain, release
sync-file, EOS가 필요합니다.

`FEATURE_DECODE_PRIME`은 MediaCodec PRIVATE AHB output을 authoritative DRM
PRIME descriptor로 내보내기 위한 별도의 실제 probe gate입니다. 기본값은
꺼짐이며, crop 메타데이터가 plane별이 아니거나 공개 plane-to-object mapping
없이 transport FD가 여러 개이거나 Debian consumer가 실제 import 및
release-fence 왕복을 완료하기 전에는 반드시 clear 상태여야 합니다.

Version 1.6은 outstanding decoded AHB output 하나의 authoritative DRM PRIME
metadata와 소유권 있는 dma-buf object FD를 전송하는 `TRANSFER_PRIME`을
추가합니다. Acquire fence는 `DEQUEUE_OUTPUT`이 반환한 것을 그대로 사용합니다.
PRIME transfer는 codec output을 retire하지 않으며 여전히 `RELEASE_OUTPUT`이
필요합니다.

Version 1.8은 `RESERVE_LINEAR`와 `ASYNC_DECODE_PRIME`을 추가합니다. Repack
gateway는 재정렬된 decoder output이 아직 없어도 안정적인 빈 LINEAR dma-buf를
반환할 수 있고, 나중에 나온 output을 GPU로 그 버퍼에 repack한 뒤 acquire fence를
content 완료 지점으로 사용합니다. Modifier별 decode/encode QCOM capability bit도
추가합니다. 이 bit는 독립적으로 gate를 통과한 transport/import capability이며
memory를 바꿔 표시하거나 QCOM을 무조건 선택하라는 뜻이 아닙니다.

Server가 지원하는 minor 0부터 현재 minor까지의 모든 request에서 reply는 해당
request minor를 그대로 사용합니다. 현재 minor보다 높은 request에는 server
현재 minor로 `UNSUPPORTED`를 반환하고 status detail에도 그 minor를 넣습니다.
Minor 3 미만의 HELLO 및 capability reply는 `BROKER_EGL_SURFACE`를 mask하며,
그 transport 생성에는 여전히 request minor 3이 필요합니다. Minor 4 미만의
reply는 `ANDROID_AHB_SURFACE`도 mask합니다. Minor 5 미만의 reply는 `DMABUF`,
`DMABUF_EGL`, `DMABUF_VULKAN`을 mask합니다. Minor 6 미만의 reply는
`DECODE_PRIME`을 mask합니다.
Minor 8 미만 reply는 `ASYNC_DECODE_PRIME`, `DECODE_QCOM_MODIFIER`,
`ENCODE_QCOM_MODIFIER`를 mask합니다.

## Operation

- `HELLO`: client가 feature mask와 최대 packet 크기를 보냅니다. Broker는
  교집합을 답합니다. 이로써 glibc client가 Android 전용 AHardwareBuffer API를
  실수로 협상하지 않습니다.
- `QUERY_CAPABILITIES`: 선택된 MIME/component 쌍과 각 component가 알려진
  hardware, software, vendor software, unknown 중 무엇인지 반환합니다.
- `CREATE_SESSION`: codec을 구성하고 시작합니다. Request에는 FD가 없고 session
  ID는 0이며, direction, width, height, bitrate, milli-Hz frame rate, flags,
  NUL-terminated MIME string, offset 88의 Android color format, offset 92의
  정확한 transport를 담은 96-byte payload를 사용합니다. 0은 legacy byte
  transport를 유지합니다. AHardwareBuffer 및 broker-local EGL Surface input은
  명시적으로 요청해야 하며 implicit fallback이 아닙니다. Decode는 color format
  0을 요구합니다. 제한된 encode subset은 `video/avc`와 `video/hevc`, 짝수
  dimension, 0보다 크고 200 Mbit/s 이하인 bitrate, 1--240 fps, Android
  `COLOR_FormatYUV420Planar`(19, tightly packed I420) 또는
  `COLOR_FormatYUV420SemiPlanar`(21, UV chroma 순서의 tightly packed NV12)만
  허용합니다. Version 1.3은 color format 0과 transport
  `BROKER_EGL_SURFACE`를 허용합니다. 이는 `COLOR_FormatSurface`를 설정하며
  client는 raw pixel을 제공하지 않습니다. Version 1.4는 color format 0과
  `ANDROID_AHB_SURFACE`를 허용합니다. Codec Surface를 사용하지만 별도 AHB
  handshake로만 pixel을 받습니다. Create flag는 0이어야 합니다. 성공 status
  reply는 할당한 0이 아닌 session ID를 header에 담습니다.
- `QUEUE_INPUT`: 압축 decoder access unit/codec-configuration buffer 또는
  tightly packed encoder frame 하나를 제출합니다. PTS는 nanosecond이며
  MediaCodec microsecond로 변환됩니다. 허용되는 flag는 `END_OF_STREAM`,
  `KEY_FRAME`, `CODEC_CONFIG`뿐입니다. Input은 48-byte fixed payload 바로 뒤의
  inline data이거나 role `INPUT_DATA`인 regular memfd 정확히 하나입니다. Input
  memfd에는 shrink, grow, write seal이 있어야 하며 offset과 size는 파일 안에
  있어야 합니다. Encoder frame은 flag 0과 정확히 `width * height * 3 / 2`
  byte를 가져야 합니다. Encoder EOS는 `END_OF_STREAM`만 가진 별도의 빈
  buffer여야 하며 client가 넣은 encoder `KEY_FRAME`과 `CODEC_CONFIG`는
  거부됩니다.
  `BROKER_EGL_SURFACE`의 일반 frame은 flag 0과 PTS를 가진 정확한 빈 inline
  packet입니다. Payload는 정확히 48 byte, data offset은 48, data size는 0,
  FD role은 `NONE`, attached FD는 없어야 합니다. Seal된 길이 0 memfd도
  거부됩니다. Broker는 결정적 단색 GLES frame 하나를 codec ANativeWindow에
  render하고 `eglPresentationTimeANDROID`로 해당 PTS를 적용한 뒤 한 번 swap합니다.
  production session에는 고정 frame 수 제한이 없으며 내부 frame sequence는
  unsigned 64-bit counter입니다. Smoke 도구는 자체적으로 제한된 frame 수를
  유지합니다. Frame sequence `i`의 RGBA는
  `(37i+17, 67i+53, 97i+101, 255) mod 256`이고 alpha는 항상 255입니다. 압축된
  frame output을 기다리는 rendered frame은 최대 하나입니다. Client가 비어 있지
  않은 encoded non-codec-configuration output을 dequeue할 때까지 다음 일반
  queue는 `WOULD_BLOCK`을 반환해 codec Surface backpressure를 제한합니다. 첫
  synchronous `eglSwapBuffers`는 공개 timeout이 없는 vendor call로 남지만 feature
  probe와 one-frame window가 위험 범위를 제한합니다. EOS는 같은 별도 empty EOS
  control packet이며 `AMediaCodec_signalEndOfInputStream`을 정확히 한 번 호출합니다.
- `DEQUEUE_OUTPUT`: payload나 FD가 없는 nonblocking request입니다. `WOULD_BLOCK`은
  decoded output이 준비되지 않았거나 output-format change를 소비했다는 뜻입니다.
  성공 시 MediaCodec byte buffer 복사본을 담은 sealed memfd 하나와 version 1.1의
  136-byte metadata payload를 반환합니다. Common field에는 output ID, PTS, size,
  flags, byte transport, dimension, Android color format, stride가 있습니다. 뒤의
  field에는 slice height와 crop rectangle이 있습니다. DRM fourcc/modifier 및
  plane metadata는 dma-buf/zero-copy 경로가 아니므로 0입니다. 안전한 byte-output
  dequeue에는 Android API 36 이상이 필요합니다. 이 버전부터 공개 NDK가 유효한
  output offset과 buffer capacity를 보장합니다. 더 오래된 runtime은 out-of-bounds
  copy 위험 대신 codec buffer를 release하고 `UNSUPPORTED`를 반환합니다. Encode의
  memfd에는 압축 AVC/HEVC byte가 들어가며 MediaCodec output flag는 ADVC
  `CODEC_CONFIG`, `KEY_FRAME`, `END_OF_STREAM`으로 mapping됩니다. Output format
  change 알림은 nonblocking으로 소비하고 `WOULD_BLOCK`으로 반환합니다. Codec
  configuration은 MediaCodec이 해당 output buffer를 낼 때만 전달합니다. 압축
  output은 raw Android color/stride/plane을 주장하지 않습니다.
  AHardwareBuffer decode session은 MediaCodec을 `AImageReader` PRIVATE Surface로
  구성합니다. Decoded image는 112-byte common metadata payload, transport
  `AHARDWAREBUFFER`, byte size 0을 반환합니다. Width, height, layers, Android
  format, stride, usage는 `AHardwareBuffer_describe`에서 얻습니다. DRM fourcc,
  modifier, plane은 0으로 두며 broker가 Qualcomm layout을 추측하지 않습니다.
  선택적 acquire-fence FD role은 `ACQUIRE_FENCE`입니다.
  Size 0인 MediaCodec Surface EOS에는 AImage가 없습니다. Broker는 rendering을
  끈 채 해당 codec buffer를 release하고 EOS 및 현재 layout을 담은 empty sealed
  byte-transport output 하나를 반환합니다. 이는 pixel을 복사하지 않고
  AImageReader를 기다리지 않는 control record입니다. 이후 dequeue는 flush까지
  `WOULD_BLOCK`을 반환합니다.
- `TRANSFER_AHB`: 정확한 8-byte output ID payload입니다. Outstanding
  AHardwareBuffer output에 한 번만 유효합니다. 성공 status reply는
  `AHB_FOLLOWS`를 담고, 바로 다음 record에서 broker가 native handle을 보냅니다.
  반복 transfer, byte-output ID, unknown ID는 거부됩니다.
- `TRANSFER_PRIME`: version 1.6 decoded-AHB export operation입니다. Attached FD가
  없는 정확한 8-byte output ID payload를 사용합니다. `DECODE_PRIME`을 협상하고
  backend가 gated export callback을 제공할 때 outstanding AHardwareBuffer decode
  output에 한 번만 유효합니다. 성공 시 8-byte status 뒤에 `REGISTER_DMABUF`가
  사용하는 정확한 256-byte dma-buf descriptor와, 선언된 각 object마다 소유권
  있는 `CLOEXEC` FD 하나를 반환합니다. Descriptor buffer ID는 요청한 output
  ID와 일치해야 합니다. 이 operation은 AHB나 acquire fence를 소비하지 않고
  codec output을 retire하지도 않습니다. Client는 반환된 object FD를 닫고 필요한
  release fence와 함께 `RELEASE_OUTPUT`도 보내야 합니다. 반복 transfer, 0/unknown
  ID, malformed descriptor, FD-count mismatch, unavailable export capability는
  fail-closed로 거부됩니다.
- `RESERVE_LINEAR`: version 1.8 gateway-local operation입니다. 정확한 PTS,
  visible width/height, NV12 fourcc를 담고 FD는 붙이지 않습니다. 성공하면 소유권
  있는 modifier-0 destination descriptor를 즉시 반환합니다. 같은 microsecond
  정규화 PTS의 최종 `DEQUEUE_OUTPUT`이 decoded content와 acquire fence를 그
  allocation에 연결합니다. 강제 QCOM pass-through 모드에서는 사용할 수 없으며
  compressed QCOM image를 선할당하지 않습니다.
- `QUEUE_AHB` / `COMPLETE_AHB`: `ANDROID_AHB_SURFACE` encoder session을 위한
  제한된 version 1.4 input handshake입니다. `QUEUE_AHB`는 정확한 PTS, width,
  height, Android format, layers, usage, flag 0, 선택적 acquire-fence role을
  담습니다. FD 없음/`FD_NONE` 또는 정확히 하나의 `ACQUIRE_FENCE` FD만 붙입니다.
  `AHB_FOLLOWS`가 있는 OK reply 후 Android client는 공개
  `AHardwareBuffer_sendHandleToUnixSocket` record를 정확히 하나 보낼 수 있습니다.
  Broker는 `AHardwareBuffer_recvHandleFromUnixSocket`으로 받고 공개 descriptor를
  request와 정확히 비교하며 native-handle integer slot을 해석하지 않습니다.
  `COMPLETE_AHB`는 render status와 FD 없음/`FD_NONE` 또는 `RELEASE_FENCE` FD
  하나를 반환합니다. Session마다 handshake는 하나만 존재할 수 있습니다. 모든
  return에서 broker가 acquire-fence duplicate를 소비하고 client가 반환된 release
  fence를 소유합니다. Native-handle record가 없거나 malformed이면 connection-fatal,
  public client가 malformed completion reply를 봐도 fatal입니다. 양쪽 모두 혼합
  protocol/native-handle stream을 resync하지 않습니다.
- `REGISTER_DMABUF`, `UNREGISTER_DMABUF`, `QUEUE_DMABUF`, `COMPLETE_DMABUF`는
  version 1.5 Debian ingress 경로를 정의합니다. Exact-import validation과
  submission callback을 모두 제공하는 backend의 `DMABUF` encoder session에서만
  dispatch합니다. Registration은 0이 아닌 client buffer ID, dimension과 crop,
  DRM fourcc, 명시적 image modifier 하나, color primaries/transfer/matrix/range/
  chroma siting, 1--4개 object, 1--4개 plane을 담은 정확한 256-byte record입니다.
  각 object는 고유 index의 attached `CLOEXEC` FD 하나와 authoritative bounded
  allocation size를 지정합니다. 각 plane은 object, 64-bit offset, 0이 아닌
  pitch를 지정합니다. Explicit fourcc, modifier, plane flag가 모두 필수입니다.
  Modifier 0은 명시적이므로 LINEAR를 의미하며 `UINT64_MAX`는 거부됩니다.
  Registration validation은 private Android handle slot을 해석하지 않고 `fstat`이
  dma-buf를 증명한다고 주장하지 않습니다. Registration 성공 전에 선택한 Android
  Vulkan 또는 EGL 구현이 정확한 descriptor를 import하고 destroy해야 합니다.
  추측한 format, 누락된 modifier, 구조 검사만으로는 부족합니다.
  `QUEUE_DMABUF`는 등록된 buffer ID, PTS, fence 없음/`FD_NONE` 또는 하나의
  `ACQUIRE_FENCE` sync-fd를 담습니다. `COMPLETE_DMABUF`는 같은 buffer를 식별하고
  성공 시에만 `RELEASE_FENCE` 하나를 반환할 수 있습니다. Buffer가 in-flight인
  동안 `UNREGISTER_DMABUF`는 금지됩니다. Android backend는 acquire sync_file을
  import하고 image를 정확히 한 번 sample해 MediaCodec input Surface로 보내며,
  마지막 source read 뒤에 순서가 보장되는 release sync_file을 반환합니다. CPU로
  raw pixel을 map/readback/copy하지 않습니다. Codec Surface가 다른 allocation을
  소유하므로 정확한 주장은 literal same-allocation pass-through가 아니라
  **CPU pixel 복사 0회와 제한된 GPU blit/draw 1회**입니다.
- `RELEASE_OUTPUT`: logical output ID와 fence role을 담은 16-byte payload입니다.
  Byte transport는 `FD_NONE`과 attached FD 없음이 필요합니다. Broker가 byte를
  복사한 직후 MediaCodec output buffer는 이미 반환되었습니다. Release는 logical
  outstanding-output record를 제거하고 client backpressure를 적용합니다.
  AHardwareBuffer output은 `FD_NONE`/FD 0개 또는 정확히 하나의 `RELEASE_FENCE`
  FD를 허용합니다. Engine은 request record가 release되기 전에 FD를 duplicate하고
  backend가 `AImage_deleteAsync`로 소비합니다.
- `FLUSH`: payload와 FD가 없으며 MediaCodec을 flush하고 session의 모든
  outstanding logical output ID를 무효화합니다. Broker-local Surface encode는
  `signalEndOfInputStream`이 one-shot이므로 `UNSUPPORTED`를 반환합니다. 대신
  session을 닫고 다시 만듭니다.
- `CLOSE_SESSION`: payload와 FD가 없으며 codec을 중지하고 삭제합니다.
- `PING`은 일반적인 request/reply 의미를 가집니다.
- 각 dma-buf backend는 독립적인 bounded device probe가 실제 dma-heap buffer를
  할당하고 exact import, GPU submission 1회, 유효한 release sync-file,
  MediaCodec Surface encode, 비어 있지 않은 compressed output, EOS를 모두 완료할
  때만 광고됩니다. 모든 retry는 완전히 새로운 codec Surface session을 사용하며
  실패하면 해당 backend bit는 clear 상태로 남습니다.

### Debian dma-buf Surface encode(version 1.5 후보)

초기 importer allowlist는 의도적으로 좁습니다. One-plane explicit-LINEAR
`ABGR8888`(`AB24`) 또는 `XBGR8888`(`XB24`), 정확히 session 크기인 crop,
RGB/full 또는 unspecified color metadata, 완전한 in-bounds object/offset/pitch
tuple만 허용합니다. 기본 backend 순서는 Vulkan 다음 EGL입니다. Vulkan에는
`VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
`VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, Android Surface 및
swapchain 지원이 필요합니다. EGL에는 `EGL_EXT_image_dma_buf_import`,
`EGL_EXT_image_dma_buf_import_modifiers`, `EGL_KHR_image_base`,
`EGL_ANDROID_native_fence_sync`, `EGL_KHR_wait_sync`가 필요하고 GLES는
`GL_OES_EGL_image_external`을 노출해야 합니다. 제공된 모든 acquire/release
fence는 header가 없는 Android NDK sysroot에서도 Linux `SYNC_IOC_FILE_INFO`
UAPI로 검증합니다. 임의의 `CLOEXEC` descriptor는 fence로 허용하지 않습니다.

Registration은 최대 object FD 4개의 duplicate를 보존하고 session마다 등록된
buffer 최대 16개, in-flight buffer 최대 4개를 허용합니다. Completion은
in-flight state를 retire하고 선택적 release fence 소유권을 client에 넘깁니다.
Session close는 등록 object와 pending completion fence를 결정적으로 닫습니다.
이 one-shot codec-Surface 경로는 `FLUSH`를 지원하지 않으므로 session을 닫고
다시 만듭니다. 격리 Vulkan 경로는 좁은 LINEAR RGB allowlist의 bounded device
probe를 통과했습니다. 테스트 기기에서는 필수 dma-buf import extension이 없어
EGL이 fail-closed로 남았습니다. 이는 설치된 production broker에 대한 주장이
아닙니다.

### Broker-local EGL Surface encode

Feature bit는 cached bounded startup probe가 실제로 320x240 AVC MediaCodec
encoder를 500 kbps, 30 fps로 구성하고, input ANativeWindow 및 recordable GLES2
EGLSurface/context를 만들고, Android presentation timestamp로 frame 하나를
render/swap하고, Surface EOS를 signal하고, bounds-checked nonempty compressed
output과 EOS를 drain하고, cleanup한 뒤에만 광고됩니다. 이후 codec별 session은
component가 요청 dimension이나 rate를 거부하면 정상적으로 실패할 수 있습니다.
Runtime API 36 미만은 codec 생성 전에 probe가 실패하며 공개 NDK byte-output
안전 gate와 일치합니다.

Pixel이 broker-local GLES rendering에서 시작해 codec Surface로 직접 가므로 이
경로에는 CPU raw-frame copy가 없습니다. 압축 encoder output은 기존 sealed byte
memfd를 사용합니다. 이는 bounded diagnostic/runtime producer이며 Debian
application zero-copy, external AHB import, 특정 GPU-to-codec 내부 layout의 증거가
아닙니다. `ANativeWindow_lock`은 사용하지 않습니다. 각 render는 자체 context와
Surface를 `eglMakeCurrent`로 다시 bind합니다. Producer는 refcount된 EGL display를
공유하므로 한 producer를 파괴해도 sibling producer가 살아 있으면 display를
terminate할 수 없습니다.

### Android AHardwareBuffer Surface encode

Version 1.4 경로는 layer 1개, 정확한 session dimension,
`AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM`,
`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`를 가진 공개 AHB descriptor만
허용합니다. Bounded Android test producer가 쓰는 sampled-image와 CPU-write
usage bit만 허용하며 protected/unknown usage는 거부합니다. Descriptor의 stride는
0이 아니고 width 이상이어야 합니다. EGL은 `EGL_ANDROID_get_native_client_buffer`,
`EGL_KHR_image_base`, `EGL_ANDROID_image_native_buffer`,
`EGL_ANDROID_native_fence_sync`, `EGL_KHR_wait_sync`를 노출해야 하고 GLES는
`GL_OES_EGL_image_external`을 노출해야 합니다. Broker는 acquire fence를 GPU
queue로 import하고 AHB를 external EGLImage texture로 bind해 codec Surface에
draw하고, 정확한 PTS를 적용하고, 한 번 swap한 뒤 해당 작업 뒤의 native release
fence를 export합니다. 이 bounded v1.4 구현은 return 전에 제출된 GPU read도
quiesce하므로 `COMPLETE_AHB` 누락, disconnect, 후속 protocol error가 client
buffer 재사용을 관측할 수 없는 fence에 의존하게 하지 않습니다. Runtime 제출은
CPU로 pixel을 map하지 않지만 GPU blit 또는 color conversion을 수행할 수 있고
direct scanout이 아닙니다.

Feature bit는 bounded local full-output probe와 production backend 대상 실제
`SOCK_SEQPACKET` v1.4 create/`QUEUE_AHB`/public-handle/`COMPLETE_AHB`/close
probe가 모두 성공한 뒤에만 광고됩니다. Native-handle receipt에는 2초 제한이
있고 handle이 없거나 malformed이면 fatal connection error입니다. Probe는 알려진
pixel을 채우기 위해서만 자체 synthetic buffer를 map합니다.

이는 Debian/glibc PRIME 경로가 아닙니다. 공개 AHB handle send/receive 함수는
Android/Bionic API이고 handle record는 glibc에 opaque로 남습니다. Debian
client에는 별도의 명시적 dma-buf fourcc/modifier/plane 계약과 EGL dma-buf
importer가 필요하며 private handle slot을 추측하지 않습니다.

`include/advc/protocol.h`의 공개 상수가 fixed payload offset의 기준입니다. C
structure를 직접 전송하지 않습니다.

## Buffer 및 fence 소유권

- Sender는 `SCM_RIGHTS`로 넘긴 FD의 소유권을 유지하고 receiver는 전달받은
  duplicate를 소유합니다.
- Version 1.1 byte output은 broker가 만든 memfd를 사용하며 shrink, grow, write,
  seal 변경이 금지되도록 seal됩니다. Receiver가 받은 FD를 소유하고 정상적으로
  닫습니다.
- Byte-mode codec output은 `AMediaCodec_releaseOutputBuffer` 전에 복사되며
  MediaCodec 또는 AHardwareBuffer reference가 socket을 건너지 않습니다.
- AHardwareBuffer mode에서는 broker가 `RELEASE_OUTPUT`, flush, close, disconnect,
  transport failure까지 `AImage`를 유지합니다. Receiver가 받은 AHardwareBuffer
  reference와 acquire-fence FD를 소유하고 backend는 duplicate된 release fence를
  소비합니다. 이는 Android/Bionic importer를 위한 실제 no-CPU-copy decode
  transport이지만 dma-buf, DRM PRIME, UBWC 주장은 아닙니다.
- Version 1.6 `TRANSFER_PRIME`이 성공하면 receiver는 반환된 dma-buf object FD
  duplicate를 소유합니다. Outstanding AHB나 dequeue acquire fence의 소유권은
  넘기지 않고 codec output도 release하지 않습니다. Client는 PRIME descriptor를
  별도로 닫고 `RELEASE_OUTPUT`으로 output 수명주기를 완료합니다.

### Debian/glibc opaque-handle 경계

`advc_glibc_receive_opaque_ahb`는 `advc_client_transfer_ahb`용 bounded receiver
callback입니다. Native-handle socket record 하나를 소비하고 payload/control
truncation을 거부하며 `SCM_RIGHTS` FD 최대 8개를 허용하고 적어도 하나의 FD를
요구하며 `CLOEXEC`을 확인합니다. `advc_glibc_opaque_ahb_close`까지 허용한 모든
descriptor를 소유합니다. 실패하면 control-truncated record의 제한된 prefix를
포함해 보이는 모든 수신 FD를 닫습니다. Matching metadata validator는 정확한
ADVC 1.2 full-allocation AHB crop, 실제 descriptor field, 유효한 선택적 acquire
fence도 요구합니다.

수신 payload는 의도적으로 opaque입니다. Android는 공개 AHardwareBuffer handle
send/receive 함수를 제공하지만 native-handle integer layout을 glibc ABI로
규정하지 않습니다. 따라서 integer slot에서 어떤 FD가 dma-buf인지, plane 순서,
DRM fourcc, modifier, UBWC 상태를 추론할 수 없습니다. `advc_glibc_ahb_to_prime`은
AHB record를 검증하고 output contract를 clear한 뒤 항상 `ENOTSUP`을 반환합니다.

별도 `advc_drm_prime_import` structure가 fail-closed AHB-conversion 계약입니다.
Fourcc, modifier, plane이 모두 authoritative로 명시된 경우에만 modifier 0을
LINEAR로 허용합니다. 누락된 metadata, invalid modifier sentinel, 사용하지 않는
nonempty plane, 잘못된 FD, zero pitch, non-CLOEXEC fence를 거부합니다. 계약 검증은
DRM import ioctl을 수행하지 않고 FD가 dma-buf임을 증명하지 않습니다. 미래의
broker-side Android mapper가 authoritative plane metadata를 제공해야 하며,
consumer EGL/Vulkan/libdrm importer도 실제 import를 수행해야 합니다. ADVC 1.2는
둘 다 하지 않으므로 아직 glibc zero-copy를 진실하게 노출할 수 없습니다.

Version 1.5는 별도의 client-originated `advc_dmabuf_descriptor` 계약을 추가합니다.
Decoder는 truncated/extended record, 0이 아닌 reserved byte, 누락된 explicit
metadata, invalid count/FD index, non-`CLOEXEC` descriptor, 0 또는 과도한 object
size, crop overflow, unknown color enum, object 밖의 first-row span, 사용하지 않는
nonempty record를 거부합니다. Bounded registry는 외부 exact-format policy
callback을 요구하고 object FD를 `CLOEXEC`으로 duplicate하며 duplicate ID를
거부하고 등록 최대 16개, 서로 다른 in-flight buffer 최대 4개를 허용합니다.
이는 source-level validation일 뿐 dma-buf 증명, EGL/Vulkan import, UBWC 주장,
feature 광고 조건이 아닙니다.

## Resource 및 scheduling 제한

- 연결된 client당 최대 session: 4.
- Session당 release되지 않은 logical output 최대: 8.
- Version 1.5 dma-buf foundation: registry당 등록 buffer 최대 16개, in-flight
  source buffer 최대 4개, 등록 buffer당 in-flight 제출 1개.
- 압축 decoder input, tightly packed raw encoder input, copied output 최대:
  16 MiB. 완전한 raw frame 하나가 이 제한을 넘으면 encode session 생성을
  거부합니다.
- 최대 protocol packet: 1 MiB. 따라서 inline input은 packet 크기로도 제한됩니다.
- MediaCodec input dequeue는 nonblocking입니다. Broker output poll 하나는 Codec2
  callback bridge에 제한된 10 ms wait 한 번을 허용한 뒤 추가 wait 없이 즉시
  사용 가능한 format/buffer-change event 최대 7개를 소비합니다. Request는 무제한
  codec wait가 될 수 없고 client는 단일 end-to-end monotonic deadline을 유지합니다.
- Client disconnect는 connection별 engine과 남은 모든 session을 파괴합니다.

## Android Codec2 output 및 codec configuration

- Decoder는 lazy configuration을 사용합니다. 첫 input이 `CODEC_CONFIG`이면 AVC
  SPS/PPS를 `csd-0`/`csd-1`에 넣고 다른 codec data는 `AMediaCodec_start` 전
  `csd-0`에 넣습니다. `ADVC_CODEC_CONFIG_AS_DATA`는 이전 queued-data 동작을
  위한 명시적 diagnostic override입니다.
- Output-format event는 coded size, color, stride, slice height, crop을 갱신합니다.
  그 event에만 들어 있는 encoder `csd-0`/`csd-1`은 이후 compressed buffer를
  drain하기 전에 bounded synthetic `CODEC_CONFIG` output 하나로 복사합니다.
- 성공한 input EOS는 flush 전까지 terminal입니다. MediaCodec이 byte-buffer
  address를 제공하지 않아도 backend는 zero-length output EOS를 mapping하며 output
  EOS 뒤에는 flush 전까지 dequeue하지 않습니다.
- Format-event draining은 request당 event 8개로 제한됩니다. 추가 event와 일반
  no-output 결과는 `WOULD_BLOCK`을 반환하고 broker는 client의 bounded polling
  protocol을 계속 처리합니다.

## Foundation의 보안 속성

Daemon은 mode `0660` socket을 bind하고 `SO_PEERCRED.uid == 0`을 검증합니다.
Packet/FD 수는 제한되며 수신 FD는 `CLOEXEC`, malformed record는 attached FD를
닫고 request ID는 echo합니다. Production integration에는 전용 group, SELinux
policy, codec watchdog timeout을 추가해야 합니다. 1.1 engine은 고정된
session/output 제한을 사용하고 unsealed input file, unknown flag, 0이 아닌
reserved field, invalid session ID, out-of-range byte region, oversized buffer,
ambiguous encoder color format, 잘못된 크기/flag의 raw frame을 거부합니다.

1.5 dma-buf 계약은 implicit format/modifier/plane metadata, protected 또는 unknown
future flag, invalid object/plane mapping, allocation 밖 first-row span, invalid
crop/color field, 누락된 `CLOEXEC`, duplicate registration, busy unregister,
registration/in-flight exhaustion도 거부합니다. 실제 importer는 정확한
fourcc/modifier allowlist를 적용하고 EGL/Vulkan import를 authoritative하게
취급해야 합니다.

## 디코드 진단 client

`advc-decode-smoke`는 제한된 decoder 검사 하나를 실행합니다.

```text
advc-decode-smoke SOCKET MIME WIDTH HEIGHT ACCESS_UNIT
```

`ACCESS_UNIT`은 16 MiB 이하의 regular file이어야 합니다. Tool은 이를 sealed
memfd로 복사합니다. `video/avc`에서는 Annex-B SPS/PPS-only prefix 뒤에 IDR
access unit이 있어야 합니다. Prefix를 먼저 `CODEC_CONFIG`로 queue하고 나머지
IDR 범위를 `KEY_FRAME`으로 queue하며 둘 다 같은 sealed memfd의 bounded range를
참조합니다. SPS/PPS 누락, malformed NAL boundary, IDR이 아닌 첫 VCL, VCL 없는
sample은 연결 전에 거부합니다. 다른 MIME type은 단일 `KEY_FRAME` input 동작을
유지합니다. 압축 input 뒤에 EOS를 보내며 polling은 broker exchange 전체에
monotonic 10초 deadline 하나를 사용합니다. 성공한 모든 byte output에서 광고된
크기와 file size가 같은 regular sealed memfd인지 검사하고, 비어 있지 않으면 첫
byte와 마지막 byte를 읽고 metadata bound를 검사하고 logical output을 release합니다.
성공에는 비어 있지 않은 output 적어도 하나와 EOS output이 필요하고 이후 session을
flush하고 닫습니다. Stdout record는 test automation용 JSON object 하나뿐입니다.

Android build는 `ADVC_SMOKE_AHB=1`을 지원합니다. 협상된 AHB/native fence
feature가 필요하고 공개 Android API로 각 handle을 받으며 같은 10초 deadline
안에서 acquire fence를 기다리고 실제 descriptor를 wire metadata와 비교한 뒤
output을 반환합니다. `ahb_outputs`는 handle, descriptor, fence, bounded lifetime
동작만 증명하며 pixel을 검증하지 않습니다.

Access-unit input은 container demuxer가 아닙니다. 성공 여부는 선택한 MediaCodec
decoder의 input framing 규칙에도 의존합니다. Byte-mode 성공은 AHB 동작을
증명하지 않습니다. AHB-mode 성공도 pixel correctness, dma-buf, UBWC, glibc
importer, encode, continuous-stream 지원을 증명하지 않습니다.

## Encoder 기기 검증 client

`advc-encode-smoke`는 제한된 AVC 또는 HEVC byte-buffer encoder session 하나를
실행합니다.

```text
advc-encode-smoke SOCKET MIME WIDTH HEIGHT i420|nv12|surface|ahb FRAMES
```

`FRAMES`는 1--120입니다. `i420`과 `nv12`에서 client는 input timestamp마다
서로 다른 deterministic tightly packed 8-bit frame을 만들고, 각 frame을 sealed
memfd로 복사하고, input backpressure 중 output을 drain하고, 별도 empty EOS를
queue하고, EOS 뒤에 session을 닫습니다. `surface`에서는 같은 client가 empty PTS
control을 보내고 producer `broker-egl-surface`를 기록합니다. Broker는 raw-frame
memfd를 할당하는 대신 GLES로 pixel을 로컬 생성합니다.

Android 전용 `ahb` 모드는 deterministic RGBA AHB 하나를 할당하고 test
producer로서만 CPU-fill합니다. V1.4 socket handshake로 공개 handle을 보내고
재사용 전에 반환된 release sync-fd를 기다리며 one-frame backpressure point를
drain한 뒤 Surface EOS를 queue합니다. Broker import/render는 CPU pixel copy를
하지 않지만 GPU blit 또는 color conversion을 할 수 있습니다. 성공에는 요청한
input마다 VCL-bearing output packet 정확히 하나, 정확한 first/last PTS,
monotonic PTS, 필수 parameter set, key VCL, EOS가 필요합니다. Pixel correctness나
Debian PRIME 지원을 주장하지 않습니다.

Nonblocking connect, negotiation, capability discovery, create, 모든
queue/dequeue/release retry, drain, close 전체에 monotonic deadline 하나를
사용합니다. 기본값은 20초이며 `ADVC_SMOKE_TIMEOUT_MS`로 1,000--60,000 ms만
선택할 수 있습니다. Output packet hard limit 2,048개가 빠른 peer가 시간 제한을
우회하지 못하게 합니다.

Session 생성 전에 client가 요청 MIME의 broker encoder capability를 query하고
선택된 component name과 보수적인 acceleration classification을 기록합니다.
`ADVC_SMOKE_REQUIRE_HARDWARE=1`은 hardware로 분류되지 않은 모든 것을 거부합니다.
`ADVC_EXPECT_CODEC_NAME`은 capability name이 정확히 일치해야 합니다. Identity는
override하지 않은 `createEncoderByType` 실행에만 authoritative합니다. ADVC 1.1은
session별 identity를 반환하지 않으므로 `ADVC_CODEC_NAME`을 쓰는 broker에는 독립
broker-log 증거도 필요합니다. Client는 capability 결과가 override를 증명하는
것처럼 가장하지 않습니다.

비어 있지 않은 compressed output은 다음 bounded 형식 중 하나로 완전히 parse되어야
합니다. Annex-B NAL unit, 4-byte big-endian length-prefixed NAL unit,
AVCDecoderConfigurationRecord, HEVCDecoderConfigurationRecord. 금지된 NAL header
bit, empty/truncated unit, length overrun, malformed parameter-set array, parse되지
않은 trailing byte를 거부합니다. 성공에는 AVC SPS+PPS 또는 HEVC VPS+SPS+PPS,
VCL unit 적어도 하나, IDR/IRAP unit 적어도 하나, EOS가 필요합니다. JSON 결과에는
component identity, acceleration, packet/flag count, syntax form, NAL/parameter-set
count, VCL timestamp bound, nonmonotonic PTS observation, total byte, FNV-1a-64
fingerprint가 포함됩니다. 이는 구조 검증 metadata이며 독립 reference decoder로
결과를 디코드하는 검증을 대체하지 않습니다.
