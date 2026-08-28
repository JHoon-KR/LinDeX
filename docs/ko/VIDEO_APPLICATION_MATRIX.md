# 비디오 앱 호환성

[English](../VIDEO_APPLICATION_MATRIX.md) | [한국어](VIDEO_APPLICATION_MATRIX.md)

이 문서는 VA-API 드라이버 탐색과 완전한 앱 재생을 구분합니다. `vainfo`가 profile을
표시해도 앱이 이를 선택하는지, 출력·EOS(end-of-stream)·seek·sandbox·fallback이
작동하는지는 증명되지 않습니다.

표는 2026-08-28 기준 Debian Trixie ARM64 패키지와 저장소의 LinDeX session을
대상으로 합니다. 패키지 빌드는 바뀔 수 있으므로 릴리스 검증에는 정확한 패키지
버전을 기록해야 합니다.

## 모듈 설치 범위

모든 profile은 FFmpeg, `vainfo`, GStreamer 도구, deprecated
`gstreamer1.0-vaapi` plugin, parser와 새 `va` plugin이 포함된
`gstreamer1.0-plugins-bad`를 설치합니다. Archcraft Sway만 mpv도 설치합니다.
LXQt와 XFCE는 mpv를 설치하지 않습니다. 어떤 profile도 VLC, Firefox ESR,
Chromium을 설치하지 않습니다.

포함된 Archcraft Sway archive에는 mpv hardware-decode 설정이 없습니다. 따라서
mpv는 upstream 기본값 `hwdec=no`를 유지합니다. LinDeX는 browser preference나
Chromium feature switch를 전역으로 설치하면 안 됩니다.

외부 broker와 `SHA256SUMS`에 기록된 모든 immutable artifact가 검증을 통과해야
session이 VA-API를 활성화합니다. 수정된 decode lifecycle과 제한된 repack resource
pool을 포함한 driver와 codec preflight 도구도 같은 manifest로 고정됩니다. H.264는
기존 reorder gate를 유지합니다. HEVC Main과 VP9 Profile 0은 공개 베타 profile로,
현재 session의 live preflight가 non-secure QTI hardware component, PRIME transport,
정확한 120/120 장기 실행 token을 모두 확인한 경우에만 광고합니다. 이 광고는 모든
player·sink·seek·sandbox lifecycle의 성공을 보장하지 않습니다.

## 현재 앱 매트릭스

| Consumer | VA-API 선택 조건 | 현재 H.264 | 현재 HEVC / VP9 | 출력 및 zero-copy 경계 | 안전한 릴리스 기본값 |
|---|---|---|---|---|---|
| FFmpeg CLI 및 FFmpeg 기반 도구 | 명시적 `-hwaccel vaapi` 또는 앱이 만든 VAAPI hardware device가 필요합니다. 일반 decode에서 CLI가 자동으로 켜지지 않습니다. | 재정렬 gate 후보가 B-frame 2개인 1280x720 H.264 Main 180프레임을 decode 오류 없이 완주했습니다. 여전히 VA-surface/null-output 결과이며 화면 출력 앱 lifecycle 통과는 아닙니다. | HEVC Main과 VP9 Profile 0은 각 live preflight 통과 시 공개 베타로 광고합니다. 둘 다 QTI hardware, PRIME, release, EOS, codec stop으로 120/120프레임을 완주했습니다. | Frame을 VA surface로 유지하면 `vaExportSurfaceHandle`에 도달할 수 있습니다. Decode `vaDeriveImage`와 `vaGetImage`가 미구현이므로 decoded surface의 system-memory download는 불가합니다. | 시험에서는 VAAPI를 명시적으로 선택하고 앱의 software fallback을 유지합니다. |
| mpv | Upstream 기본값은 `hwdec=no`입니다. `--hwdec=vaapi,auto`가 direct VAAPI를 opt-in하고 초기화 실패 시 mpv software fallback을 남깁니다. | Direct VAAPI는 후보이며 앱 통과가 아닙니다. `--hwdec=vaapi-copy`는 미구현 download 경로에서 이미 실패했습니다. | Live preflight 뒤 profile은 보이지만 화면 HEVC/VP9 재생과 seek는 공개 베타 시험 대상입니다. | `vaapi`는 호환 GPU video output과 export된 NV12 dma-buf/modifier import가 필요합니다. QCOM UBWC와 repacked LINEAR를 따로 검증해야 합니다. `vaapi-copy`는 zero-copy가 아니며 현재 미지원입니다. | 아직 mpv.conf override를 추가하지 않습니다. |
| VLC | VLC 3는 `avcodec-hw`가 hardware decode를 허용할 때 libavcodec VAAPI/DRM hardware context를 사용합니다. LinDeX는 VLC를 설치하지 않습니다. | 미검증입니다. 사용자가 VLC를 설치하면 광고된 H.264 VLD를 자동 시도할 수 있습니다. | Capability gate를 통과한 profile을 볼 수 있으나 앱 lifecycle은 미검증입니다. | Direct video-output과 extraction/download는 별개입니다. QCOM/LINEAR import, Wayland 표시, seek, drain이 통과하지 않았고 download는 누락된 decode image API에 의존할 수 없습니다. | VLC preference를 만들지 않고 전용 실기 통과 전까지 software/default 동작을 유지합니다. |
| Firefox ESR | GTK build에는 decoded frame을 dma-buf로 내보내 Gecko/WebRender에 전달하는 FFmpeg VAAPI 경로가 있습니다. 선택은 Firefox hardware decode policy와 runtime blocklist에도 좌우됩니다. LinDeX는 Firefox를 설치하지 않습니다. | 설치된 v3.0.3-dev에서 seek 30회와 reload 10회, RDD seccomp mode 2, RDD KGSL FD 0, `c2.qti.avc.decoder`, decoded 766/dropped 5, hardware output 1,757회, LINEAR export 1,758회, clean teardown/restart를 통과했습니다. | HEVC/VP9은 live preflight 뒤 광고하지만 Firefox에서의 해당 profile 화면 재생·seek 증거는 아직 없습니다. | 범위가 제한된 선연결 socket gateway가 QCOM import와 Vulkan repack을 RDD 밖에서 수행하고 Firefox에는 LINEAR dma-buf와 fence를 전달하므로 sandbox를 약화하지 않습니다. | Sandbox와 전용 launcher를 유지하고 HEVC/VP9 browser 재생은 공개 베타로 취급합니다. |
| Debian Chromium ARM64 | Compile에서 빠진 backend는 runtime flag로 켤 수 없습니다. Debian Trixie ARM64 Chromium rule은 `use_vaapi=false`이고 V4L2를 사용합니다. LinDeX는 Chromium을 설치하지 않습니다. | Stock Debian ARM64 package에서는 불가합니다. | 같은 이유로 ADVC를 사용할 수 없습니다. | Upstream Linux VAAPI는 dma-buf/DRM 기반이지만 이 package에는 포함되지 않습니다. 별도 ARM64 build에는 독립 sandbox와 native-pixmap modifier 검증이 필요합니다. | 건너뜁니다. 효과 없는 flag나 sandbox 약화 flag를 추가하지 않습니다. |
| GIMP 3 | GIMP는 정지 이미지 loader와 GEGL 연산을 사용하며 설치된 실행 파일은 LinDeX VA-API 비디오 driver를 소비하지 않습니다. | 해당 없음. GIMP는 H.264/HEVC 재생·인코드 검수 대상이 아닙니다. | 해당 없음. | 이미지 import/export가 CPU 또는 GPU filter를 사용할 수는 있지만 ADVC/MediaCodec video surface를 검증하지 않습니다. | 비디오 코덱 매트릭스에서 제외합니다. |
| GStreamer `playbin`/`decodebin` | Decoder factory rank로 자동 연결될 수 있습니다. 기존 `vaapi*` plugin과 새 `va*` plugin은 서로 다른 구현입니다. | 명시적 old `vaapih264dec ! video/x-raw(memory:VASurface) ! fakesink` pipeline은 이제 downstream/bus EOS와 clean exit를 통과합니다. 실패 원인은 software fallback이 아니라 미선언 dma-buf cleanup이 GStreamer FD 0을 닫은 것이었습니다. `playbin`/`decodebin`과 새 `vah264dec`는 미검증입니다. | HEVC Main은 `c2.qti.hevc.decoder`로 120/120, VP9 Profile 0은 `c2.qti.vp9.decoder`로 120/120프레임을 완주했습니다. 둘 다 QCOM NV12 입력, 프레임당 Vulkan repack 1회, LINEAR export, 명시적 EOS와 release를 사용했습니다. 이 결과로 live preflight 통과 시 공개 베타 profile을 광고합니다. | 이 격리 통과는 `playbin` 화면 표시나 seek 증거가 아닙니다. 새 `VAMemory`/DMABuf 출력과 실제 화면 재생은 별도 검증 대상입니다. | 자동 decoder rank는 유지합니다. Capability 광고는 자동 앱 decode 성공 증거가 아닙니다. |

## 하드웨어 코덱 테스트 앱

실행 중인 LinDeX profile 안에서 아래 도구를 사용합니다. 앱 버전, codec/profile,
출력 정책(`auto`, `linear`, `qcom`), 해상도, frame 수, seek 수, decoded/dropped,
EOS, 종료 뒤 고아 broker session 여부를 함께 기록해야 합니다.

1. **목록 확인 — `vainfo`:** `vainfo --display drm --device
   /dev/dri/renderD128`을 실행합니다. H.264 CB/Main은 보여야 하고 HEVC Main과 VP9
   Profile 0은 해당 live preflight가 통과한 경우에만 나타납니다. 목록만 보인 것은
   재생 성공이 아닙니다.
2. **디코드 전송 — FFmpeg:** `ffmpeg -hwaccel vaapi -hwaccel_device
   /dev/dri/renderD128 -hwaccel_output_format vaapi -i INPUT -f null -`을 H.264
   Main+B, HEVC Main, VP9 Profile 0 파일에 각각 실행합니다.
3. **화면 재생 — mpv:** `mpv --hwdec=vaapi,auto INPUT`으로 반복 seek, EOS,
   재재생, 창 크기 변경, 종료를 시험합니다. Decode system-memory download가
   미구현이므로 `vaapi-copy`는 사용하지 않습니다.
4. **자동 pipeline — GStreamer:** `gst-inspect-1.0`으로 사용 가능한 `vaapi*` 또는
   `va*` factory를 기록한 뒤 `gst-launch-1.0 playbin
   uri=file:///ABSOLUTE/PATH/INPUT`을 시험합니다. Codec/EOS와 Wayland sink/import
   실패를 분리하기 위해 명시적 VASurface/fakesink 실행도 남깁니다.
5. **Sandbox 재생 — Firefox ESR:** LinDeX launcher로 local H.264/HEVC/VP9 fixture를
   열고 `about:support`, seek, reload, WebRender 상태, decoded/dropped 수를 기록합니다.
   RDD 또는 content sandbox를 끄면 안 됩니다.
6. **인코드 — FFmpeg와 OBS:** FFmpeg에서는 VAAPI hardware device와 `h264_vaapi`
   또는 `hevc_vaapi`, OBS에서는 광고된 VAAPI H.264/HEVC encoder를 선택합니다.
   60초 인코드 후 정상 종료하고 결과 파일의 frame 수, timestamp, A/V 길이를
   디코드하여 확인합니다.

Android 쪽 확인에는 선택된 `c2.qti.*` component가 포함된 LinDeX session log와
MediaCodec metrics를 함께 저장합니다. Decoder 이름, `vainfo` 항목, 부드러워 보이는
영상 하나만으로는 충분한 증거가 아닙니다.

정적 분류에 사용한 자료:

- [mpv hardware decode manual](https://mpv.io/manual/stable/#options-hwdec)
- GStreamer [hardware decode 및 feature-rank 안내](https://gstreamer.freedesktop.org/documentation/tutorials/playback/hardware-accelerated-video-decoding.html),
  Debian Trixie [gstreamer-vaapi 1.26.2 source](https://sources.debian.org/src/gstreamer-vaapi/1.26.2-1/)
- Firefox [VAAPI dma-buf frame pool](https://sources.debian.org/src/firefox-esr/140.12.0esr-1~deb13u1/dom/media/platforms/ffmpeg/FFmpegVideoFramePool.h/)
- Chromium [Linux VA-API 문서](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/gpu/vaapi.md),
  Debian Trixie [ARM64 build rule](https://sources.debian.org/src/chromium/150.0.7871.100-1~deb13u1/debian/rules/)
- Debian Trixie [VLC 3 source package](https://sources.debian.org/src/vlc/3.0.23-0%2Bdeb13u1/)

## 저장소 session과 대조

외부 broker와 `SHA256SUMS`의 모든 immutable artifact가 preflight를 통과하면 현재
session은 `LIBVA_DRIVER_NAME=advc`와 H.264 decode gate를 desktop session 전체에
export하고 GStreamer registry scan도 process 내부로 설정합니다. 함께 포장된
`advc-vaapi-decode-preflight`는 현재 gateway에 HEVC Main과 VP9 Profile 0을 각각
질의합니다. PRIME transport, non-secure QTI hardware identity 또는 정확한 120/120
token 중 하나라도 없으면 해당 profile만 광고하지 않습니다. 외부에서 전달한 앱
승인 변수는 무시합니다.

남은 릴리스 blocker는 화면 mpv/playbin/VLC/browser lifecycle과 seek입니다. HEVC와
VP9의 120/120 transport gate는 앱 화면/seek gate를 대신하지 않습니다. 명시적
old-VAAPI GStreamer H.264 VASurface/fakesink EOS는 통과했지만 자동 앱 경로는 아직
아닙니다. 초기 환경 정리는 새 `ADVC_VAAPI_ENABLE_HEVC`,
`ADVC_VAAPI_ENABLE_VP9`, `ADVC_VAAPI_GPU_LINEAR_REPACK`,
`ADVC_VAAPI_DECODE_OUTPUT`, `ADVC_VAAPI_ENCODE_INPUT`, deprecated
`ADVC_VAAPI_OUTPUT`을 명시적으로 unset합니다. 과거 앱 검증 ACK 변수도 지우므로
상속된 driver 개발 값만으로 gate를 열 수는 없습니다.

릴리스 runtime은 장치 lifecycle 검증 뒤에만 변경해야 하며 이 문서는 현재 동작을
조용히 바꾸지 않습니다.

## Fail-closed 자동 선택 설계

하나의 전역 약속 대신 capability gate와 앱 시험 매트릭스를 사용합니다.

1. **Runtime-ready gate:** immutable driver/broker 조합, socket, render node,
   procfs FD view, DRM sysfs view, 정확한 codec capability를 검증합니다. HEVC Main과
   VP9 Profile 0은 각각의 제한된 live check가 통과한 경우에만 공개 베타로 광고합니다.
2. **앱 증거:** parser, inter-frame stream, output import, EOS, seek, teardown,
   software fallback을 앱별로 추적합니다. 앱 증거가 없으면 공개 주장을 좁혀야 하지만
   live preflight가 보고한 hardware capability를 임의로 만들거나 숨기지는 않습니다.

앱 launcher는 runtime-ready gate를 다시 검사하고 자식에 필요한 최소 환경만
설정할 수 있습니다. Preflight 실패 시 모든 ADVC selector를 제거하고 앱을 정상
software mode로 시작해야 합니다. 전역 mpv/VLC/browser preference를 수정하면 안
됩니다. 중간 frame에서 backing memory를 조용히 바꿔 hang을 안전하게 복구할 수는
없습니다. Hardware session을 파괴하고 앱이 software로 playback을 다시 시작해야
합니다.

권장 승격 순서:

1. 명시적 VA-surface-to-PRIME 출력과 EOS를 포함한 FFmpeg H.264
2. QCOM UBWC와 repacked LINEAR를 분리한 mpv H.264 direct output
3. 기존/새 VA plugin을 별도 행으로 둔 GStreamer; 각 전체 실행 통과 전 decoder
   rank는 `NONE`
4. 기본 fallback 동작을 유지한 VLC
5. RDD/GPU sandbox 및 dma-buf modifier를 검증한 뒤 Firefox
6. VA-API가 compile-out된 stock Debian Chromium ARM64는 계속 제외

HEVC Main은 multi-frame inter-picture VAAPI/PRIME/EOS를 통과해 공개 베타 profile로
광고합니다. VP9 Profile 0도 key/inter transport를 통과해 같은 기준으로 광고합니다.
화면 표시와 seek는 계속 호환성 시험 대상입니다. AV1은
[코덱 및 zero-copy 상태](VIDEO_ZERO_COPY_STATUS.md)에 설명한 표준 VAAPI parameter
계약 한계 때문에 계속 미지원입니다.

## Zero-copy 및 EOS 주장 경계

- VA surface가 자동으로 표시 가능한 dma-buf가 되는 것은 아닙니다.
- QCOM UBWC는 consumer가 정확한 modifier/layout을 import할 때만 CPU raw-pixel
  copy 0회입니다. LINEAR로 바꿔 표시할 수 없습니다.
- Repacked LINEAR는 Vulkan image copy 1회, CPU raw-pixel copy 0회입니다. 호환
  경로이며 decoder direct allocation은 아닙니다.
- `vaapi-copy`, system-memory GStreamer caps, screenshot, filter, browser fallback은
  `vaGetImage`/`vaDeriveImage`를 요청할 수 있고 decoded surface는 현재 이를
  지원하지 않습니다.
- Frame count 또는 `vainfo` 성공은 EOS 증거가 아닙니다. 각 앱 gate는 parser EOS,
  delayed-frame drain, downstream EOS, surface/fence release, context destroy, broker
  orphan 0을 모두 보여야 합니다.
