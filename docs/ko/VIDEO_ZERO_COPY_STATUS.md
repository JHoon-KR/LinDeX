# 코덱 및 zero-copy 상태

[English](../VIDEO_ZERO_COPY_STATUS.md) | [한국어](VIDEO_ZERO_COPY_STATUS.md)

마지막 갱신: 2026-08-28 (Asia/Seoul)

## 주장 범위

LinDeX에는 ADVC라는 실험적 Android MediaCodec 브리지가 있습니다. 현재 공개
릴리스 주장은 의도적으로 다음 범위로 제한합니다.

- 집중 검증한 기준 기기 인코드 후보는 Vulkan으로 Debian dma-buf를 가져와
  **CPU raw-pixel 복사 0회**, MediaCodec 입력 Surface로 **제한된 GPU blit
  1회**를 수행했습니다.
- byte 및 Android 로컬 AHardwareBuffer/Surface 경로에는 개발 단계 기능 증거가
  있습니다.
- 기준 기기 byte decode는 720x360 NV12, color format 21, stride 768,
  slice height 384, output 436,176 byte, raw-frame CPU 복사 1회로 통과했습니다.
- 격리 디코드 후보가 연속 QCOM UBWC PRIME 120프레임, EOS, teardown,
  Turnip offscreen content 및 acquire/release-fence lifecycle을 통과했습니다.
  형식은 NV12 fourcc `0x3231564e`, modifier `0x0500000000000001`, object 1개,
  plane 2개이며 raw-pixel CPU 복사는 0회입니다.
- 동일한 120프레임이 gate를 거친 QCOM UBWC AHB→명시적 `modifier = 0`
  LINEAR dma-buf Vulkan image-to-image repack도 통과했습니다. 프레임당 GPU
  repack 1회, CPU raw-pixel 복사 0회이며 이미 명시적 LINEAR인 원본은 repack을
  우회합니다.
- 설치된 정확한 모듈에서 제한된 FFmpeg·GStreamer AVC/HEVC 인코드와 OBS AVC·
  HEVC texture 녹화를 통과했으며, 공개 릴리스 태그에서 반복해야 합니다.
- 디코드 광고는 계속 fail-closed입니다. H.264 Constrained Baseline/Main, HEVC
  Main, VP9 Profile 0은 각 정확한 검사 뒤에만 노출합니다. HEVC Main과 VP9 Profile
  0은 live preflight가 QTI component, PRIME transport, 성공한 120/120 token을 확인한
  경우에만 공개 베타로 광고합니다. AV1은 표준 VA-API가
  정확한 복원에 필요한 원본 bitstream syntax를 충분히 전달하지 않아 숨깁니다.

“CPU raw-pixel 복사 0회”는 “작업 0회”가 아니며 코덱 내부 변환도 없다고 보장하는
표현이 아닙니다. GPU blit 1회 경로는 항상 그대로 설명해야 합니다.

## 2026-08-28 장기 실행 및 병목 감사

v3 패키징 전에 기준 기기 격리 매트릭스를 확대했습니다. H.264 Main+B는
180/180프레임을 147.28fps, HEVC Main+B는 120/120을 118.73fps, VP9 Profile 0은
120/120을 108.46fps로 완주했습니다. 모든 경우 decoder 출력은 QCOM modifier
`0x0500000000000001`, 목적지는 명시적 modifier-0 LINEAR였고 Vulkan image repack
1회, CPU raw-pixel 복사 0회, EOS, 정상 codec stop을 확인했습니다. 평균 repack
시간은 각각 1.379ms, 1.619ms, 1.738ms였습니다.

H.264 Main+B는 bounded reorder 계약이 없으면 출력이 전혀 나오지 않았습니다.
격리 장기 매트릭스는 처음에 4-frame 재구성 상한을 사용했지만 화면 Firefox
ESR/RDD 검증에서 더 작은 제출 창이 확인됐습니다. 상한 4와 2는 첫 export를
막았고 0은 순서를 깨뜨렸으며, 상한 1만 재생과 반복 seek를 완주했습니다.
따라서 패키지 session은 전체 코덱 사전 검사를 통과한 뒤 정확히
`ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1`만 내보냅니다. H.264
High, HEVC Main10, AV1은 계속 광고하지 않습니다. HEVC Main과 VP9 Profile 0은
각 독립 live preflight 뒤 공개 베타로 광고하며, 화면 재생과 seek는 앱별 호환성
증거로 계속 수집합니다.

KernelSU로 설치한 v3.0.3-dev 패키지 ZIP을 실험용 Android low-latency codec
switch 없이 일반 production gateway로 검증했습니다. Firefox RDD는 seccomp
mode 2와 KGSL FD 0개를 유지했고 seek 30회, document reload 10회를 완료하여
decoded frame 766, dropped frame 5를 기록했습니다. VA driver trace는 hardware
output 1,757회, LINEAR export 성공 1,758회, export timeout 0회, 비동기 실패
0회를 기록했으며 Android media metrics는 `c2.qti.avc.decoder`와 low-latency
mode off를 확인했습니다. 활성 session을 종료한 뒤 Firefox, gateway, Sway,
session-runner 고아는 없었고 다음 Sway session도 정상 시작했습니다. 공개 release
ZIP의 정확한 tagged-release 반복과 gateway의 프레임별 fence 계수 감사는 남아
있습니다.

같은 시점의 720p60 인코드 감사에서는 QTI 하드웨어 인코더로 H.264 Constrained
Baseline 120/120을 84.9fps, HEVC Main 120/120을 81.1fps로 완주했습니다. 일반
FFmpeg upload는 프레임당 CPU raw-pixel 복사 1회와 GPU 변환 1회, PRIME/OBS
경로는 CPU raw-pixel 복사 0회와 동일한 GPU 변환 1회로 집계했습니다. VP8·VP9·
AV1 하드웨어 인코더는 없어서 숨깁니다. 인코드 입력은 명시적 LINEAR NV12이며
압축 QCOM 입력을 허용하거나 LINEAR로 가장하지 않습니다.

## VA-API 인코드 이정표

2026-08-26 기준 기기의 격리 환경에서 실제 FFmpeg `h264_vaapi` 및
`hevc_vaapi` 세션이 ADVC 드라이버와 QTI MediaCodec component를 거쳐
완주했습니다. 이후 정확한 모듈이 아래의 제한된 앱 매트릭스를 반복했습니다.

- `vaSyncBuffer`를 구현하고 vtable에 등록했습니다. FFmpeg가 사용하는 기능
  프로브 `vaSyncBuffer(display, VA_INVALID_ID, 0)`는
  `VA_STATUS_ERROR_INVALID_BUFFER`를 반환하므로, FFmpeg는 매 프레임
  `vaSyncSurface`로 폴백하지 않고 비동기 coded-buffer 경로를 선택합니다.
- Vulkan Surface producer는 이제 `VK_GOOGLE_display_timing`에 절대 monotonic
  display 시각을 제공합니다. 0 ms나 33 ms 같은 media PTS는 유효한
  `desiredPresentTime`이 아닙니다. 이를 그대로 사용한 것이 첫 IDR 출력 뒤 QTI
  Surface 인코드가 멈춘 원인이었습니다. 논리 media PTS는 broker의 순서 보장
  frame-token 매핑에 별도로 유지하며 coded output에서 복원합니다.
- H.264 Constrained Baseline과 HEVC Main은 모두 FFmpeg `async_depth` 2, 3, 4에서
  5프레임을 통과했습니다. Depth 3에서는 둘 다 마지막 pending tail
  (`2 -> 1 -> 0`)까지 30/30프레임을 완주했고 software decode도 30프레임 전부를
  복구했습니다. Android media metrics도 `c2.qti.avc.encoder`와
  `c2.qti.hevc.encoder`의 일치하는 hardware 세션을 독립적으로 기록했습니다.
- 새 registry를 사용한 GStreamer에서 두 코덱 모두 완주했습니다. H.264
  Constrained Baseline은 5/5프레임(21,982 byte, EOS 68 ms)과 30/30프레임
  (126,978 byte, EOS 644 ms), HEVC Main은 5/5프레임(20,717 byte, EOS 72 ms)과
  30/30프레임(140,132 byte, EOS 710 ms)을 통과했습니다. `ffprobe`와 software
  decode는 모든 프레임을 복구했습니다. ADVC userspace call log에서도
  `vaSyncSurface`, coded-buffer map/unmap, `DestroyContext` 반환이 모두
  성공했고 Android media metrics는 QTI AVC/HEVC 30-frame hardware 세션을
  독립적으로 기록했습니다.
- 이 일반 GStreamer/FFmpeg 앱 실행은 별도 gate인 generic NV12 upload 경로를
  사용했습니다. CPU-origin 이미지를 LINEAR dma-buf에 쓰고 그 버퍼에서 hardware
  encode를 진행합니다. 이는 앱의 hardware codec 작동 증거이지만 zero-copy 입력
  주장은 아닙니다. 명시적으로 import한 PRIME/texture 경로만 그 주장을 할 수
  있습니다.
- OBS는 격리 headless Wayland 세션에서 제한 시간 탐색과 실제 H.264 texture
  경로 녹화를 모두 완료했습니다. H.264용 `ffmpeg_vaapi_tex`와 HEVC용
  `hevc_ffmpeg_vaapi_tex`를 노출하고 AV1은 거부했으며, 동일 writable VA surface를
  독립 PRIME FD로 반복 내보냈습니다. 녹화는 과거 실수로 남아 있던 120-frame
  runtime 제한을 넘어 output 363프레임에서 정상 중지했습니다. `ffprobe` 결과는
  H.264 Constrained Baseline, 320x240, `yuv420p`, 30 fps, 12.10초였고 software
  decode가 363프레임 전부를 복구했습니다. OBS log에는 `Recording Stop`과
  성공한 `DestroyContext`가 남았으며 generic-upload 폴백, export 실패, encode
  오류는 없었습니다.
- 정확한 패키지 모듈도 KernelSU 활성화 후 production broker를 통해 같은 경로를
  반복했습니다. 12.133초 동안 364/364프레임을 복구했고 writable export 369회,
  정상 `StopRecord`/`DestroyContext`, export·fallback·EndPicture·encode 오류 0,
  OBS·Sway·DBus 고아 프로세스 0을 확인했습니다.
- 설치 모듈의 `hevc_ffmpeg_vaapi_tex`도 HEVC Main 364/364프레임, 12.133초,
  writable export 369회로 같은 clean-stop/no-fallback 조건을 통과했습니다.
  명시적 OBS H.264 VBR 프로필은 362/362프레임과 writable export 367회를
  완료했습니다. Android media metrics는 실제 hardware component가
  `c2.qti.hevc.encoder` profile 1과 `c2.qti.avc.encoder` profile 65536이고 두
  세션 모두 `bitrate_mode=VBR`임을 확인했습니다.
- 인코더 광고 범위는 장치 전체 codec 목록보다 의도적으로 좁습니다. ADVC는
  H.264 Constrained Baseline과 HEVC Main만 노출합니다. OBS는 H.264 Main/High,
  HEVC Main10, AV1을 거부했고 FFmpeg도 사용 가능한 VA-API 인코더 두 개만
  열거했습니다. Protocol v1은 VBR만 허용하며 Android backend도 MediaCodec
  `bitrate-mode=VBR`을 명시적으로 설정합니다. 정확한 진단용 CBR override는
  테스트 전용이고 광고하지 않습니다.
- GStreamer 탐색은 chroot에 `/proc/self/fd`, `/sys/class/drm`,
  `/dev/dri/renderD128`이 노출되고 `GST_VAAPI_DRM_DEVICE`로 해당 render node를
  명시했을 때만 열려야 합니다. `/proc`와 `/sys`가 없으면 libdrm이 FD를 render
  node로 분류하지 못하며 libva는 ADVC driver까지 도달하지 않습니다. 이
  Android/chroot 조합의 forked plugin scanner는 큰 VA-API feature reply를 잃고
  `libgstvaapi.so`를 blacklist했습니다. `GST_REGISTRY_FORK=no`로 registry scan을
  process 내부에서 수행하고 `GST_VAAPI_ALL_DRIVERS=1`을 함께 사용하면
  `vaapih264enc`·`vaapih265enc`와 VBR rate-control 속성을 정상 열거합니다. 두
  변수는 동일한 fail-closed ADVC runtime gate가 성공한 경우에만 export합니다.
- 집중 검증한 imported dma-buf 인코드 입력 경로는 CPU raw-pixel 복사 0회,
  프레임당 MediaCodec 입력 Surface로의 Vulkan GPU 변환/blit 1회입니다. 이
  설명에는 위 generic CPU-origin upload 실행이 포함되지 않습니다. Direct
  scanout이나 GPU 작업 0회 경로는 아닙니다.

### 인코드 입력 복사 계수 계약

두 앱 입력 경로의 계수는 의도적으로 다릅니다.

- `vaPutImage` generic upload는 성공한 입력 한 번마다 논리적 CPU raw-pixel
  복사 1회를 수행합니다. NV12는 plane별 row 복사, I420은 추가 U/V→NV12
  interleave를 수행합니다. 구현은 monotonic copy counter를 노출하며, 테스트는
  성공한 upload만 정확히 1 증가하고 거부된 upload는 증가하지 않음을 확인합니다.
- Writable DRM PRIME export는 descriptor FD를 복제하지만 pixel byte를 복사하지
  않습니다. OBS는 `EndPicture` 전에 동일 VA surface의 소유 descriptor를 여러 번
  요청할 수 있으므로 반복 export는 별도로 셉니다. `EndPicture`가 마지막 implicit
  producer fence를 snapshot한 뒤 surface를 broker GPU 변환/blit 1회에 제출합니다.
- 두 경로를 모두 사용한 프레임은 `mixed`로 보고하며 CPU-copy 0으로 표현하면 안
  됩니다. Trace accounting은 CPU copy, writable export, 성공한 GPU conversion
  submit을 따로 기록합니다. Driver 내부에는 PRIME write-export에서 generic upload로
  조용히 전환하는 fallback이 없습니다.

정상 Vulkan 동작은 절대 monotonic scheduling입니다. 이전의 logical PTS를
display 시각으로 쓰는 동작은 정확한 진단 게이트
`ADVC_VULKAN_LEGACY_PRESENT_PTS=diagnostic-only-v1`에서만 재현할 수 있으며
릴리스 세션에는 사용하면 안 됩니다. 성공한 GStreamer H.264 실행 약 1분 뒤,
저수준 ftrace 수집이 아직 활성화된 상태에서 기기가 재부팅되었습니다. 이후
`last_kmsg`는 shell 요청 재부팅과 임시 루트 exploit 프로세스
`cve43499-rootho`의 `Bad page state` BUG를 기록했습니다. 이는 이미 EOS와
context 해제를 마친 코덱 세션보다 루트 복구 경로의 kernel memory corruption을
가리키는 증거입니다. 이와 별개로 해당 tracing 방식은 폐기합니다. 실제 OBS
인코드는 격리된 정확한 driver/broker 조합과 설치된 release 모듈에서 모두
통과했고 host release verifier도 이 정확한 payload를 고정합니다. Runtime 광고는
필수 mount, device node, socket identity, manifest digest가 모두 통과하지 않으면
계속 fail-closed입니다.

## VA-API 디코드 후보 이정표

2026-08-26 격리 디코드 이정표는 처음에 broker `678d72f9…df4211`과
200,488-byte 드라이버 `2dca711a…492e4`를 사용했습니다. 이후 descriptor 소유권
수정을 반영해 200,600-byte 드라이버 `bd8c8a05…f5e70`을 재현 빌드하고
2026-08-27 release snapshot에 설치했습니다. 아래 transport 결과는 이 정확한
드라이버에서도 반복했으며 200,488-byte 파일은 과거 증거로만 유지합니다.

- Direct QCOM UBWC는 H.264 Main nonempty 120/120프레임과 EOS를 완료했습니다.
  Broker는 EOS 포함 output 121개, AHB/PRIME export 120회, acquire/release-fence
  왕복 120회, release call 121회를 반환했습니다. Turnip이 모든 export frame의
  content와 crop을 확인했고 modifier는 `0x0500000000000001`이었습니다.
- Gate를 거친 LINEAR 경로도 동일한 120/120프레임과 EOS를 완료했습니다. 120개
  descriptor 모두 명시적 `modifier = 0`이었고 Vulkan repack은 프레임마다
  destination acquire fence와 source release fence를 반환했습니다. LINEAR content
  hash는 direct UBWC 검증 hash와 일치했습니다. 구현 및 trace 기준 프레임당 Vulkan
  image-to-image repack 1회, CPU raw-pixel 복사 0회입니다.
- 기준 MediaCodec/AImageReader에 original LINEAR allocation을 요청해도 QCOM UBWC
  modifier가 반환됐습니다. 따라서 이를 LINEAR로 바꿔 표시하지 않습니다. 이 기기의
  명시적 LINEAR 경로는 original decoder buffer가 아니라 GPU-repacked destination입니다.
- Runtime gate는 정확히
  `ADVC_VAAPI_GPU_LINEAR_REPACK=validated-qcom-nv12-v1`이어야 합니다. Gate 누락,
  잘못된 값, LINEAR source, 잘못된 QCOM metadata는 fail-closed입니다.
  `ADVC_VAAPI_DECODE_OUTPUT=qcom`도
  `ADVC_VAAPI_QCOM_IMPORT=validated-v1`을 요구하므로 output selector가 검증된
  modifier 정책을 우회하지 못합니다. Deprecated `ADVC_VAAPI_OUTPUT`은 방향별 새
  selector가 unset일 때만 사용합니다.

앱 증거는 transport probe와 구분합니다.

- 이전 `vainfo` 실행은 H.264 Constrained Baseline/Main VLD만 열거했습니다. Broker는 QTI
  HEVC/VP9/AV1 decoder component를 보고합니다. 격리 HEVC Main I/P/B와 VP9
  Profile 0 key/inter는 이제 VA-API transport, QCOM PRIME, release, EOS, codec
  stop을 통과했습니다. 검증 전용 HEVC gate도 decode 순서 `0,1,3,2`를 POC에
  연결하고 Turnip GPU readback한 NV12 crop 네 개를 POC 정렬한 software decode와
  byte-exact 비교해 통과했습니다. Stock session은 각 정확한 live preflight가
  통과할 때만 HEVC Main과 VP9 Profile 0을 공개 베타로 광고합니다. 화면 앱
  lifecycle과 seek는 외부 승인 token이 아니라 남은 호환성 시험입니다. AV1은 더
  근본적인 이유로 숨깁니다.
  FFmpeg 표준 AV1 VA-API 경로는 원본 OBU stream 대신 header가 제거된 tile data를
  전달하고 public VA parameter에도 정확한 OBU 복원에 필요한 syntax가 빠져 있습니다.
  이 bit를 추측하면 거짓 hardware-decode 계약이 됩니다.
- FFmpeg는 VA-API hardware decode를 선택하고 최종 격리 후보에서 H.264 Main
  30프레임을 완료했습니다. 이는 VA surface 경로이며 null output은 30개 surface
  모두에 DRM PRIME export를 강제하지 않으므로 PRIME lifecycle 증거로 쓰지 않습니다.
- GStreamer는 새 registry에서 `vaapidecodebin`, `vaapih264dec`, `vaapisink`를
  열거했습니다. 명시적 GStreamer 1.26 old-VAAPI H.264
  `memory:VASurface ! fakesink` pipeline은 4프레임을 모두 디코드하고 downstream·
  bus EOS, 제한 시간 내 종료, codec output 폐기 0, destroy drain 비활성,
  codec stop status 0을 통과했습니다. 이전 timeout 원인은
  `advc_dmabuf_descriptor_close()`가 미선언 0 값 slot까지 닫아 GStreamer FD 0의
  bus/poll wakeup handle을 파괴한 것이었습니다. Cleanup을 `object_count`가 선언한
  고유 object로 제한했고 host 회귀 테스트도 추가했습니다. 이 좁은 통과에서는
  PRIME import, video sink, playbin/decodebin, seek, 화면 표시를 실행하지 않았으므로
  일반 GStreamer 앱 디코드 성공으로 확대해서는 안 됩니다.
- mpv `--hwdec=vaapi-copy`는 H.264 VA-API decoder를 선택한 뒤 의도적으로 미구현인
  `vaGetImage` CPU-download 경로에서 실패했습니다. 이번 실행은 DP 출력을 쓰지
  않았으므로 실제 GPU zero-copy mpv VO는 검증하지 않았습니다.

120-frame smoke/content 실행 파일과 fixture, HEVC EOS/POC hook, NV12 hash helper는
검증 전용입니다. 실행 파일·hook·sample·fixture 모두 release 모듈에 추가하면 안
되며 release verifier가 이름과 경로로 거부합니다.

## 모듈의 앱 광고 연결

모듈은 이제 ARM64 libva vendor driver를
`payload/debian/codec/advc_drv_video.so`에 포함하고 모듈 산출물 manifest로
검증한 뒤 chroot의
`/opt/android-drm-lease-kit/codec/vaapi/advc_drv_video.so`에 immutable하게
설치합니다. Release verifier는 ELF64 AArch64 형식과 일치하는 SHA-256 manifest를
요구합니다. Smoke 및 합성 테스트 실행 파일은 계속 source/development 산출물이며
release ZIP에는 들어갈 수 없습니다.

v3.0.3 source module의 코덱 산출물은 다음과 같습니다.

- broker: 141,480 byte, SHA-256
  `34d8a0dbfa7c3f15ffb3ddfc3c419c951610b043323caf5db2c4d3bb253f2954`
- capability probe: 140,352 byte, SHA-256
  `caa424606c7544ab45e50020b639e6c31eb9e5e960e798abd79303579d76a8be`
- VA driver: 200,312 byte, SHA-256
  `5c695313ebd8bf88693b94fb05eaa08b38b147cefad876b4d1035894940d9973`
- decode preflight: 67,576 byte, SHA-256
  `dbe634d840bffebdb85afc3f9402fe2754e8b3d1072a13f0ce5e191f227111a9`
- repack gateway: 133,192 byte, SHA-256
  `acc31bbd9c31673ebeacfb468d41baf0244271d4610dc462d77b677aae52668f`
- Firefox RDD socket adapter: 67,344 byte, SHA-256
  `3b29a65894fac042c5c8f480623569bc8516e3ec27300783466591033ab502e9`

재현 가능한 ARM64 빌드, host video 15/15, VA-API 13/13, 모듈 통합, 프로필,
WebUI, hotplug, 패키지 검증과 VA-API 14/14를 통과했고 최종 release/dev ZIP 후보에 포함했습니다.
다만 이 조합을 기준 기기에 함께 설치하지는 않았습니다. 아래 설치 snapshot은
과거 활성화 증거입니다.

- broker: 126,768 byte, SHA-256
  `678d72f908955bf7a1be11976199deaf7bf63b06e56b4d9e42d56fb3c6df4211`
- driver: 200,600 byte, SHA-256
  `bd8c8a059cd150f512ae517ba4cc08c49b100ca796a3e624d72581818d1f5e70`

이전 134,520-byte 드라이버 `0bfac348…d064c3`와 아래 통합 ZIP은 과거 인코드
증거로만 유지합니다. 기준 기기에서 그 driver는 profile 3개, image format 2개로
loader/vtable gate를 통과했습니다. 위
인코드 앱 매트릭스는 격리 GStreamer H.264/H.265와 OBS texture 363-frame 녹화를
포함합니다. SHA-256
`56b5d6dcd4db8377b76a6daaba0f8b33fa544379572b6ab7aa78e96ef4fe51e5`인 당시 통합
ZIP은 기준 기기에 설치·활성화되어 FFmpeg H.264/HEVC 인코드 30/30, GStreamer
H.264/H.265 인코드 5/5, OBS software decode 검증 364/364를 반복 통과했고
production broker PID도 유지했습니다. 이 ZIP은 현재 query-status, multi-slice,
HEVC, VP9 디코드 소스 변경보다 이전 산출물이므로 현재 트리의 릴리스 파일이
아닙니다.

`VIDEO_ACCELERATION=auto`일 때 외부 launcher가 먼저 정확한 identity 검사를 거친
production broker를 reconcile합니다. 내부 세션은 driver digest, broker socket,
render node, procfs FD view, DRM sysfs view를 독립적으로 모두 확인한 뒤에만 다음
client selector와 exact validation gate를 내보냅니다.

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

`VIDEO_ACCELERATION=disabled`, 잘못된 policy, 또는 파일·mount·device·manifest·
socket 중 하나라도 누락되면 모든 selector와 validation gate를 unset 상태로
유지합니다. Generic-upload gate는 CPU-origin NV12를 위한 정상 compatibility
경로이며 OBS texture/PRIME zero-copy 경로가 아닙니다. OBS texture write-export
경로는 동일 surface 반복 export와 decode 363프레임으로 격리 live gate를
통과했습니다. 따라서 정확한 gate
`ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1`는 패키지된
driver/broker manifest와 runtime preflight가 모두 통과한 경우에만 내보낼 수
있으며, 그 외에는 unset 상태를 유지합니다. Desktop은 정상 software codec
fallback으로 시작할 수 있지만 LinDeX가 ADVC를 부분적으로 광고하지는 않습니다.

## 전송 상태

| 방향 | 전송 | 현재 상태 | 릴리스 표현 |
|---|---|---|---|
| 인코드 | byte 입력 | 기능성 호환 경로 | 입력 byte를 복사하며 zero-copy 아님 |
| 인코드 | Android 로컬 AHB/Surface | 개발 단계 기능 경로 | Android 로컬 하드웨어 경로 |
| 인코드 | Vulkan을 통한 Debian dma-buf | 격리 live gate와 설치 모듈 OBS H.264/HEVC texture 녹화 통과 | CPU raw-pixel 복사 0회, GPU blit 1회, 공개 릴리스 태그에서 반복 |
| 인코드 | EGL을 통한 Debian dma-buf | 집중 프로브에 사용한 기준 스택에서 미노출 | 자체 live probe가 통과할 때만 지원 |
| 디코드 | byte 출력 | 기준 기기 PASS: 720x360, color format 21/NV12, stride 768, slice height 384, 436,176 byte | Raw-frame CPU 복사 1회, 정확한 release 산출물에서 반복 |
| 디코드 | private AHB 출력 | 기준 QTI decoder가 YUV_420_888/CPU usage 요청에도 QCOM-compressed output 반환 | Android 로컬 출력이며 direct LINEAR 주장이 아님 |
| 디코드 | DRM PRIME LINEAR(`modifier = 0`) | 격리 후보 PASS: 프레임당 Vulkan image-to-image repack 1회로 120/120+EOS, destination content가 direct UBWC와 일치, CPU raw-pixel 복사 0회. 기준 기기에서 original decoder LINEAR는 계속 미지원 | 정확한 드라이버는 source module에 고정됨. 패키지 플레이어 화면 재생과 seek 통과 전까지 더 넓은 repack gate는 unset 유지 |
| 디코드 | DRM PRIME QCOM UBWC | 격리 후보 PASS: 120/120+EOS, content/crop, acquire/release fence, 반복 release, teardown. NV12 `0x3231564e`, modifier `0x0500000000000001`, object 1개/plane 2개, raw-pixel CPU 복사 0회 | H.264 QCOM import는 정확한 gate와 전체 module preflight 뒤에만 광고 가능 |

엄격한 zero-copy 모드는 live probe를 통과한 backend만 허용합니다. Byte 전송이나
CPU raw-pixel 복사로 조용히 바뀌지 않습니다. Auto 모드는 세션 생성 시 문서화된
폴백을 선택할 수 있지만 프레임 도중 전환하지 않습니다.

## Descriptor 및 modifier 규칙

PRIME descriptor는 DRM fourcc, 명시적 modifier 의미, allocation/object 수,
plane-to-object 대응, offset, stride, logical crop, 소유한 파일 디스크립터/fence를
모두 제공할 때만 authoritative합니다.

Modifier 값 `0`에는 특별하지만 모호하지 않은 규칙이 있습니다.

- Descriptor의 explicit-modifier flag가 설정된 경우에만 `0`은 유효한
  **DRM LINEAR**입니다.
- 명시적 선언이 없는 `0`은 누락되거나 모호한 메타데이터이므로 거부해야 합니다.
- QCOM UBWC는 정확한 0이 아닌 modifier와 검증된 보조/데이터 layout을 제공해야
  하며, 기기 계열이나 버퍼 크기에서 추론하면 안 됩니다.

디코더와 importer는 `modifier = 0` LINEAR도 0이 아닌 modifier만큼 엄격하게
검증해야 합니다. Plane 범위, object 크기, 형식, crop, acquire fence, 내용,
release fence가 모두 필수입니다. 명시적 LINEAR 계약은 단위 수준에서 다룹니다.
기준 Samsung/QTI 기기에서 YUV_420_888과 CPU usage를 요청해도 QCOM-compressed
output이 반환되어 direct original-buffer `modifier = 0` 경로를 증명하지
못했습니다. 따라서 공개 매트릭스는 이 unavailable direct allocation과 검증된
one-Vulkan-repack LINEAR destination을 구분합니다.

## 디코드 PRIME 광고 경계

디코드된 private AHardwareBuffer에는 vendor별 plane 및 압축 메타데이터가 있을
수 있습니다. 파일 디스크립터 내보내기만으로 Debian consumer가 이미지를 정확히
해석한다고 증명할 수 없습니다. 120-frame QCOM UBWC와 GPU-repacked LINEAR 결과는
좁은 H.264 경로 두 개를 증명하지만 일반 decoder capability를 정당화하지 않습니다.
광고는 계속 다음 조건에 묶여야 합니다.

1. private vendor C++ ABI 없이 stable mapper 메타데이터 디코드
2. 정확한 image object 선택과 소유권 있는 PRIME FD 전송
3. 정확한 QCOM gate, 또는 정확한 LINEAR repack gate와 해당 Vulkan runtime
4. logical crop을 사용한 GPU 내용 검증
5. 비동기 consumer 작업 시 acquire-fence wait와 실제 release-fence 왕복
6. 반복 release, malformed descriptor 거부, 연결 해제 정리, 세션 재생성
7. 필수 검사 중 하나라도 실패할 때 capability 미광고

설치된 200,600-byte 드라이버는 제한된 H.264 QCOM transport lifecycle을 충족하고
검증된 LINEAR repack 구현도 포함합니다. 다만 정확한 패키지 플레이어 화면 재생과
seek가 통과할 때까지 stock session의 더 넓은 LINEAR gate는 unset입니다. Byte,
VA-surface-only, Android-local 디코드 증거를 DRM PRIME 검증으로 바꾸어 표현하면
안 됩니다. HEVC는 격리 pixel gate를 통과했지만 application gate까지 숨깁니다.
VP9은 자체 pixel/application 증거까지 숨기고 AV1은 의도적으로 지원하지 않습니다.

## 릴리스 패키징 경계

릴리스 모듈에는 검토된 런타임 broker/service, ARM64 VA-API driver, live gating에
필요한 제한된 capability probe만 포함합니다. 그 밖의 진단 및 합성 실행 파일과 입력
스트림/로그는 개발 산출물이며 공개 모듈 ZIP에 포함하면 안 됩니다.

재현성을 위해 소스 테스트는 저장소에 남을 수 있습니다. 소스에 존재한다는 것이
모듈에 패키징할 권한을 의미하지 않습니다.

## 최종 코덱 기록 양식

최종 패키지 모듈 결과마다 [검증 상태](VALIDATION_STATUS.md)에 다음 내용을
포함한 행을 추가합니다.

- Git commit, 모듈 ZIP SHA-256, 빌드 flavor, 기기 모델, Android 버전
- 코덱 component와 입력/출력 전송
- 프레임 수, nonempty 출력, 총 byte, EOS, 제한된 실행 시간
- CPU raw-pixel 복사 횟수와 GPU 작업 횟수
- 관련되는 형식, modifier, object/plane layout, crop, fence 결과
- 반복 실행 및 teardown 결과
- 패키지 모듈 증거와 격리 프로브 증거의 명시적 구분

이 정확한 기록을 확보하기 전에는 최종 수치를 공개하지 않습니다.
