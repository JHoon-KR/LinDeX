# 디코드 실패 집중 분석

해결된 문제와 남은 디코드 실패를 소유권 경계별로 분리한 문서입니다. 실기
검증을 통과한 gate만 LinDeX 기본 세션에서 켜며, 해결되지 않은 진단 gate는
계속 비활성으로 유지합니다.

## 1. H.264 IDR에서 P-frame으로 전환

VA-API의 H.264 picture parameter에는 PPS 기본 활성 참조 개수가 없습니다.
따라서 LinDeX는 기본값을 override하지 않는 첫 P/B slice가 제출될 때에야
그 값을 복원할 수 있습니다. 기존 경로는 이 시점에 같은 PPS id를 다시
생성해 P-frame보다 먼저 별도 `CODEC_CONFIG` 입력으로 보냈습니다. Qualcomm
MediaCodec이 이를 스트림 중간 재구성 경계로 지연 처리하면, 관찰된
“IDR 성공 후 P-frame 지연 또는 마지막 정체”와 일치합니다.

격리 실기 게이트는 현재 H.264 Main 4프레임, 프레임당 slice 4개, PRIME
export, PTS 순서, EOS, surface release 및 codec stop을 통과했습니다. Broker의
스트림 중간 `CODEC_CONFIG` queue는 0회였으므로 in-band 후보는 이 fixture에서
실행되지도 필요하지도 않았습니다. 실제 PPS 변경 스트림이 아래 게이트를
증명할 때까지 이 후보는 꺼 둡니다.

`ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE=validated-pps-v1`

실기 통과 조건은 IDR과 P-frame 모두 출력, PTS 순서 보존, terminal EOS
도달, 예상 PRIME descriptor export입니다.

## 1a. H.264 Main B-frame 재정렬 메타데이터

초기 SPS 재구성은 VUI 전체를 생략했습니다. 위의 B-frame 없는 fixture에는
충분하지만 VA-API가 원본 SPS를 노출하지 않으므로 원본
`max_num_reorder_frames` 계약을 보존하지 못합니다. 기준 Qualcomm decoder에서
B-frame 2개인 320x240 Main stream은 입력을 받아들이면서 출력을 돌려주지 않아
결국 input window가 찼습니다. 같은 320x240 및 1280x720 경로에서 B-frame을 0으로
하면 완주했으므로 해상도나 PRIME allocation 문제와 분리됩니다.

fail-closed gate는 화면비, 색, timing, HRD를 추측하지 않고 VUI의
bitstream-restriction 부분만 생성합니다. 격리 디코드는 처음에 4-frame 상한을
검증했지만 화면 Firefox의 제출 창에는 1-frame 상한이 필요했습니다.

`ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1`

격리 상한에서 B-frame 2개의 320x240 stream은 30/30, 1280x720 browser fixture는
FFmpeg decode 오류 0회로 180/180 및 16.6배속을 완주했습니다. 실제 Firefox
ESR/RDD에서는 상한 4와 2가 첫 필수 export 전에 멈췄고 0은 표시 순서를
깨뜨렸습니다. 상한 1은 설치된 v3.0.3-dev production gateway로 seek 30회와
document reload 10회를 완료했습니다. Firefox는 decoded 766, dropped 5 frame을,
VA driver는 hardware output 1,757회, export 성공 1,758회, export timeout 0회,
비동기 실패 0회를 기록했습니다. RDD는 seccomp mode 2와 KGSL FD 0개를 유지했고
Android는 `c2.qti.avc.decoder`를 선택했습니다. 활성 session 종료 뒤 Firefox,
gateway, Sway, session-runner 고아도 없었습니다. 기본 session은 이제 검증된
1-frame gate를 내보내며 공개 tagged release ZIP 반복과 gateway fence 계수 감사는
남아 있습니다.

## 2. 앱 EOS 계약

표준 VA-API에는 vendor driver에 “압축 입력 종료”를 알리는 범용 호출이
없습니다. 따라서 `vaSyncSurface()`, `vaQuerySurfaceStatus()` 또는 context
파괴를 EOS로 추측하면 안전하지 않습니다. private EOS ABI는 이제 dequeue
소유자를 하나로 통일하고 terminal 상태를 결정적으로 처리하지만, 여전히
명시적으로 선택하는 계약입니다.

현재 private ABI 1.0만으로는 안전한 GStreamer finish hook을 만들 수
없습니다. plugin이 준비된 `VASurfaceID`, 멱등적인 surface별 release 함수,
release fence 소유권을 함께 받아야 합니다. 이 정보 없이 progress를
강제하면 broker의 8개 lease가 고갈되거나 프레임 순서가 틀어질 수
있습니다. 따라서 GStreamer 선행 패치는 fail-closed release hook만
추가하며 stock 앱 통합 완료로 간주하지 않습니다.

## 3. VASurface EOS와 프로세스 종료

기존 `memory:VASurface ! fakesink` 실행은 모든 입력을 디코드하고 sink EOS까지
게시했지만 프로세스가 timeout되었습니다. 원인은 MediaCodec stop이나 비워지지
않은 `AImageReader`가 아니었습니다. `advc_dmabuf_descriptor_close()`가
`object_count=0`인데도 object slot 4개를 모두 훑었고, 사용하지 않은 0 초기화
slot 때문에 `close(0)`을 호출해 GStreamer bus/poll wakeup FD를 닫았습니다.
GstBin은 이미 모든 sink의 EOS를 확인했지만 bus callback이 깨어날 수 없었고
main loop는 `POLLNVAL`에서 회전했습니다.

이제 cleanup은 `object_count`가 선언한 고유 FD만 닫고, producer는 실패 가능한
작업 전에 연속된 FD 소유권을 먼저 게시해야 합니다. 회귀 테스트는 zeroed
descriptor가 FD 0을 닫지 않고 미선언 slot도 건드리지 않음을 증명합니다.

구 드라이버/신 드라이버 A/B에서 명시적 H.264
`memory:VASurface ! fakesink` pipeline은 downstream·bus EOS와 제한 시간 내
종료를 모두 통과했습니다. Codec output 폐기 0, destroy drain 비활성,
동일 세션의 `AMediaCodec_stop()` status 0도 확인했습니다. 따라서 destroy-drain
후보 가설은 기각되었고 계속 비활성 상태여야 합니다.

## 4. GStreamer registry 스캔

`gst-plugin-scanner`를 종료하는 것은 codec 검색 해결책이 아닙니다. 부분
registry가 남아 정상 VA plugin이 blacklist로 재생될 수 있습니다.
LinDeX는 `gst_init` 전에 ADVC preflight를 수행하고 세션 전용 registry와
`GST_REGISTRY_FORK=no`를 사용합니다. scanner를 전역 종료하면 안 됩니다.

## 릴리스 전 실기 게이트

- H.264: multi-slice 및 IDR→P, frame/PTS/EOS/PRIME 검사
- HEVC: one-IRAP와 4-frame Main I/P/B inline-RPS가 transport, PRIME, EOS,
  stop을 통과함. 검증 전용 hook이 surface ID를 POC에 연결하고 정확한 PRIME
  allocation을 Turnip으로 읽어 POC 정렬한 software decode의 NV12 crop 네 개와
  byte-exact 일치함
- VP9: Profile 0 key+inter가 PRIME, release, EOS, stop 통과
- GStreamer: 명시적 old-VAAPI H.264 VASurface pipeline은
  `gstreamer-h264-vasurface-eos-teardown-pass`를 요구함. playbin/decodebin,
  seek, 화면 표시, 자동 decoder 선택은 별도 게이트임
- 독립 검증된 코덱이 하나의 드라이버를 공유하도록 검토된 runtime 코드는
  패키지할 수 있지만, 미검증 capability는 각 target-device 게이트를 통과할
  때까지 광고하지 않음
