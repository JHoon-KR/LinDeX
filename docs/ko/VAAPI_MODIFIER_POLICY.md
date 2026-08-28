# VA-API modifier 정책

LinDeX의 양방향 기본값은 `auto`입니다. `auto`는 fail-closed입니다. Descriptor에
실제 QCOM modifier가 있고 broker가 해당 probe capability를 광고하며 정확한 importer
검증 gate까지 있을 때만 QCOM compressed dma-buf를 직접 사용합니다. 그 외에는
명시적 modifier-0 LINEAR dma-buf를 안전 경로로 사용합니다. UBWC를 LINEAR로 바꿔
표시하지 않으며 modifier 폴백에서 CPU로 픽셀을 복사하지 않습니다.

## 설정과 이전 방법

| 방향 | 변수 | 값 | 기본값 |
|---|---|---|---|
| 디코드 출력 | `ADVC_VAAPI_DECODE_OUTPUT` | `auto`, `linear`, `qcom` | `auto` |
| 인코드 입력 | `ADVC_VAAPI_ENCODE_INPUT` | `auto`, `linear`, `qcom` | `auto` |

`ADVC_VAAPI_OUTPUT`은 deprecated 호환 alias로 남습니다. 방향별 새 변수가 비어 있지
않으면 항상 우선합니다. 알 수 없는 값은 초기화 오류이며 `auto`로 해석하지 않습니다.

`qcom`은 진단용 강제 모드입니다. 모든 direct-import gate가 통과하지 않으면
unsupported 오류로 실패하며 조용히 폴백하지 않습니다. `linear`은 외부 encode 입력에서
modifier `0`만 허용하고 decode 출력은 항상 LINEAR를 요구합니다. OBS와 일반 VA
producer에는 쓰기 가능한 상호운용 surface가 필요하므로 내부 할당 encode surface는
강제 `qcom`을 포함한 모든 모드에서 LINEAR를 유지합니다.

검증 token은 selector와 의도적으로 분리되어 있습니다.

- decode consumer: `ADVC_VAAPI_QCOM_IMPORT=validated-v1`
- encode broker importer: `ADVC_VAAPI_ENCODE_QCOM_IMPORT=validated-v1` 및 broker의
  `ADVC_ENCODE_QCOM_IMPORT_VALIDATION=validated-turnip-qcom-nv12-surface-v1`
- GPU fallback: `ADVC_VAAPI_GPU_LINEAR_REPACK=validated-qcom-nv12-v1`

Selector는 capability를 만들지 않고 독립적으로 검증된 capability 중 하나만 고릅니다.

## 데이터 흐름

| 작업과 원본 | `auto` 결과 | 강제 모드 | CPU 픽셀 복사 |
|---|---|---|---|
| protocol 1.8 async/B-frame decode | QTI UBWC → Turnip/Vulkan → 선할당 LINEAR. Export는 즉시 반환하고 `vaSyncSurface`가 content/fence를 완료 | `linear`도 동일. `qcom`은 async reservation을 끄고 direct QCOM 미검증 시 실패 | 0 |
| 실제 LINEAR 동기 decode | LINEAR direct | `qcom`은 거부 | 0 |
| 실제 QCOM 동기 decode | broker+consumer gate가 모두 있으면 direct, 아니면 GPU repack 1회로 LINEAR | gate가 빠진 `qcom`은 실패 | 0 |
| 내부 할당/OBS writable NV12 encode | LINEAR PRIME을 broker Vulkan producer로 전달하고 MediaCodec 입력 Surface에 GPU render 1회 | 모든 selector에서 LINEAR 유지 | 0 |
| 외부 LINEAR DRM PRIME encode | broker Vulkan producer로 direct | `qcom`은 외부 LINEAR import 거부 | 0 |
| 외부 QCOM NV12 DRM PRIME encode | descriptor+broker+검증 gate가 있으면 direct import, 아니면 기존 broker render 전에 QCOM→LINEAR GPU repack 1회 | `linear`은 QCOM 거부, `qcom`은 direct 불가 시 repack 대신 실패 | 0 |

Encode 출력은 H.264 또는 HEVC byte stream입니다. 압축 bitstream 자체에는 DRM
modifier가 없습니다. Modifier는 encode 전 또는 decode 후 교환하는 image memory만
설명합니다.

## Gateway route

Repack gateway 기본값은 `ADVC_REPACK_GATEWAY_OUTPUT=auto`입니다. Protocol 1.8 async
LINEAR reservation을 광고하므로 B-frame 안전 경로는 LINEAR입니다. Reservation이
없는 QCOM frame은
`ADVC_REPACK_GATEWAY_QCOM_PASSTHROUGH=validated-downstream-import-v1` 뒤에만
pass-through할 수 있습니다. `ADVC_REPACK_GATEWAY_OUTPUT=qcom`도 이 token을 요구하고
async LINEAR reservation을 끄며 LINEAR source에는 unsupported를 반환합니다. 따라서
안전하지 않은 QCOM 선할당이 발생하지 않고 선택 이유가 log에 남습니다.

## 앱 사용

권장 session 기본값:

```sh
export ADVC_VAAPI_DECODE_OUTPUT=auto
export ADVC_VAAPI_ENCODE_INPUT=auto
export ADVC_VAAPI_ASYNC_EXPORT=candidate-firefox-bframe-v1
```

비동기 token은 format 선택값이 아니라 버전이 지정된 검증 gate입니다. Protocol 1.8
선할당 LINEAR decode surface를 사용하려면 이 값이 필요하며, 없으면 드라이버는 의도대로
기존 동기 export 경로를 유지합니다.

- OBS는 일반 NV12 VA encode surface를 만들고 writable DRM PRIME export를 요청합니다.
  LinDeX는 modifier-0 LINEAR를 반환하고 `EndPicture`에서 producer dma-buf fence를
  snapshot하며 CPU 픽셀 복사 0회를 보고합니다.
- FFmpeg와 GStreamer는 표준 VA-API hardware device, VA surface, DRM PRIME descriptor
  API를 사용합니다. 방향별 selector는 격리 시험에만 설정하고 배포 권장값은
  `auto`입니다.
- Decode downstream importer가 정확한 QCOM modifier를 해석한다고 검증되기 전에는
  LINEAR를 유지합니다. Decoded surface의 system-memory download에는
  `vaDeriveImage`/`vaGetImage`가 필요한데 현재 이 driver는 구현하지 않았습니다.

FFmpeg에서 `-hwaccel vaapi -hwaccel_output_format vaapi`는 decoded frame을 VA
surface로 유지합니다. `hwdownload`를 추가하면 현재 미지원인 CPU download API를
선택합니다. `format=nv12,hwupload`를 쓰는 일반 encode chain은 지원하지만 CPU-origin
upload이므로 OBS/DRM-PRIME zero-copy 사례가 아닙니다. GStreamer는 pipeline 전체에서
`memory:VASurface`/VA memory를 유지해야 합니다. 일반 system-memory
`video/x-raw`를 협상하면 upload/download가 허용되어 copy accounting이 달라집니다.
기존 `vaapi*`와 새 `va*` plugin 계열은 설치 여부와 element 이름이 다르므로 registry
발견만 앱 통과로 보지 말고 정확한 pipeline을 각각 검증해야 합니다.

현재 근거는 정책 unit test, host VA/video suite, ARM64 build, protocol 1.8 H.264
B-frame/no-B 및 HEVC decode smoke, H.264/HEVC hardware encode smoke입니다. 외부 QCOM
encode direct import는 새 gate 뒤에 구현했지만 기준 기기 앱 실행은 아직 통과하지
않았으므로 release profile에서 켜면 안 됩니다.
