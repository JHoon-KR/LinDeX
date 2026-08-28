# 검증 상태

[English](../VALIDATION_STATUS.md) | [한국어](VALIDATION_STATUS.md)

마지막 갱신: 2026-08-28 (Asia/Seoul)

이 페이지는 v3 릴리스 후보의 공개 증거 원장입니다. 비공개 기기 작업 로그보다
의도적으로 짧습니다. pending으로 표시된 행은 해당 산출물과 기기 결과를 여기에
기록하기 전까지 릴리스 주장으로 올리면 안 됩니다.

## 기준 대상

- 기기: Samsung Galaxy S25 Edge, `SM-S937N`
- GPU: Adreno 830
- 검수에 사용한 root 관리자: KernelSU
- Rootfs: Debian 13(Trixie), ARM64, `/data/local/debian`
- 물리 출력: USB-C DisplayPort Alt Mode

이는 기기 중심 통합입니다. 결과가 다른 Qualcomm 세대, Android 빌드, root
관리자, 독을 자동으로 지원한다는 뜻은 아닙니다.

## 릴리스 매트릭스

| ID | 영역 | 현재 증거 | 공개 상태 | 완료에 필요한 작업 |
|---|---|---|---|---|
| V-01 | 소스 빌드 및 호스트 테스트 | 현재 브리지, 비디오, WebUI, 패키지/provider, lifecycle 테스트 통과 | 현재 후보에서 통과 | 정확한 태그에서 재실행하고 CI URL 보존 |
| V-02 | 공개 소스 경계 | ignore 규칙이 rootfs, 다운로드, 로그, 과거 실험, 빌드 트리를 제외 | 저장소 감사 통과 | 태그에서 manifest 및 secret scan 재실행 |
| V-03 | Android 오버레이 없음 | 모듈 트리와 설치된 모듈에 `system/` payload가 없고 런타임 정책이 Android 읽기 전용 파티션 쓰기를 금지 | 소스·패키지·설치 트리 검사 통과 | 릴리스 태그에서 재확인 |
| V-04 | Release/dev 로그 | release는 영구 진단 스트림을 남기지 않고 dev는 제한된 로그를 순환. 재현 가능한 v3.0.1 후보 검증 통과: release `d62e3fe4…19fbc5`(70,555,862 byte), dev `d40a9c00…e75cf`(70,555,857 byte) | 소스/패키지 검사 통과. `module.prop`/`flavor.conf` 외 payload 64개는 byte-identical | 두 패키지 ZIP을 기기에서 확인 |
| V-05 | 설치 및 활성화 | DP 분리 상태에서 release ZIP `2e79f952…12beb`을 설치하고 임시 루트 기준 기기에서 `ksud soft-reboot`로 활성화. chroot 마운트 6개·release flavor·production broker·200,600-byte driver·payload hash 확인. 이후 source-only session cleanup 변경이 있어 이는 최종 공개 ZIP이 아닌 설치 검증 snapshot | 기준 기기 snapshot 통과 | 최종 소스를 다시 패키징하고 영구 루트 기기에서 공개용 일반 재부팅 절차 반복 |
| V-06 | Archcraft Sway Free | 공식 공개 자산 source lock/checksum, LinDeX aggregate, Dark/Light/Pywal 계약 및 Debian 안전 어댑터 통과 | 최종 기기 매트릭스 대기 | 시작, 중지, 분리, 재연결과 각 외형, 배경화면, Waybar, 조정된 제어 확인 |
| V-10 | LXQt + labwc | 명시적 구성요소/PCManFM-Qt 및 PolicyKit 에이전트 제외 계약 통과 | 최종 기기 매트릭스 대기 | 시작, 바탕화면 아이콘, 중지, 분리, 재연결 |
| V-11 | XFCE + labwc | 단일 lease compositor, stock `xinitrc`, 패널, 바탕화면 아이콘, 독, 터미널 실기 확인. 패키지 launcher는 Xfce가 두 번째 중첩/창모드 labwc를 시작하지 못하게 함 | 부분 통과 | 최종 ZIP에서 중지, 강제 분리, 물리 재연결 뒤 새 시작 반복 |
| V-12 | 인코드 dma-buf/AHB | 기존 FFmpeg/GStreamer/OBS 매트릭스에 더해 2026-08-28 격리 720p60 감사에서 QTI 하드웨어 H.264 CB 120/120 84.9fps, HEVC Main 120/120 81.1fps 완주. Generic upload는 CPU pixel 복사 1회와 GPU 변환 1회, PRIME/OBS는 CPU pixel 복사 0회와 GPU 변환 1회 | Vulkan producer는 정체성 검증 import cache, 제한된 in-flight slot 4개를 사용하고 정상 경로 queue-idle 직렬화가 없음. EGL dma-buf와 Android 로컬 AHB producer도 제한된 native-fence cache를 사용하도록 구현함. AHB 정체성은 동적으로 찾은 API 31의 시스템 전체 고유 ID, 자체 보유 buffer 참조, 정확한 descriptor를 함께 사용함. native fence나 ID API가 없거나 ID 조회가 실패하면 의도적으로 동기식 경로를 유지함. Android NDK ARM64 빌드, 정적 분석, API 28 심볼 감사, host 15/15 통과. 최종 AHB cache의 장치 실시간 성능 검증은 아직 수행하지 않음 | 최종 release ZIP에서 두 코덱과 OBS를 반복하고 장치에서 AHB cache probe 및 제한된 종료 증거 확보 |
| V-13 | 디코드 byte | 기준 기기 PASS: 720x360, color format 21/NV12, stride 768, slice height 384, 436,176 byte, raw-frame CPU 복사 1회 | 기준 기기 결과 확인 | 정확한 release 산출물에서 반복하고 hash/EOS/정리 기록 |
| V-14 | 디코드 QCOM UBWC PRIME | 확대 격리 매트릭스에서 H.264 Main+B 180/180 147.28fps, HEVC Main+B 120/120 118.73fps, VP9 Profile 0 120/120 108.46fps로 QCOM PRIME, release, EOS, codec stop 완주. H.264는 정확한 bounded reorder gate가 없으면 출력 0개에서 고착 | 전체 사전 검사 뒤 H.264에 정확한 검증 reorder gate를 내보냄. HEVC Main과 VP9 Profile 0은 각 live QTI/PRIME/120-of-120 검사 뒤 공개 베타로 광고. H.264 High, HEVC Main10, AV1은 숨김 | release ZIP에서 앱별 GPU 화면 재생, seek, 종료 반복 |
| V-15 | Modifier `0` LINEAR | 장기 실행에서 QCOM modifier `0x0500000000000001`을 명시적 modifier `0` LINEAR로 변환하고 동일 content, destination/source fence, Vulkan image repack 1회, CPU raw-pixel 복사 0회 확인. 평균 repack 1.379~1.738ms | 최종 gateway는 제한된 source/destination 32-slot pool, descriptor+fstat 정체성 cache, lease-token 소유권, 재사용 전 release-fence wait를 추가. Host 13/13 및 동일한 ARM64 빌드 2회 통과. Original decoder LINEAR는 미지원 | 각 공개 베타 profile을 최종 package의 화면 재생·seek로 반복 |
| V-16 | 직접 스캔아웃 | 원본 Debian `libwlroots-0.18 0.18.2-3` 및 Sway `1.10.1-2` 기준 기기 PASS: UBWC 우선에서 동일 lease GETFB2 91회 중 88회가 XB24/QCOM `0x0500000000000001`, 3회는 진입·종료 LINEAR 전환. Waybar 복원 시 XR24 LINEAR 복귀. 별도 커서 무이동 시험은 89회 중 87회가 XB24/QCOM. Waybar가 매핑된 상태의 GETFB2는 정확히 XR24/modifier `0`이며 top-layer buffer 때문에 modifier 협상 전에 scene이 부적격해짐 | 호환성 우선 `auto`는 stock wlroots의 XR24 선택을 바꾸지 않고 후보만 추가하므로 90/90 XR24 LINEAR. 이는 Waybar가 UBWC를 끄는 것이 아니라 정상적인 합성 폴백 | 정확한 release 산출물에서 반복하고 선택형 ID 기반 Sway IPC 전체화면 bar watcher 검증. 항상 표시되는 압축 합성 데스크톱이 필요할 때만 버전별 wlroots 어댑터 또는 패치 사용 |
| V-17 | USB 입력 소유권 | 세션 제한 구현 및 호스트 검사 존재 | 독 매트릭스 대기 | 키보드, 마우스, 터치패드, 기타 USB, 휴대폰 터치, 중지, 재연결 |
| V-18 | 강제 분리 소유권 | Leader 우선 종료 고아 정리, 토큰 검증, 첫 sample 강제 분리, Type-C 한정 시작 보호, 연속 2회 안정 재연결 호스트 fixture 통과. 현재 기준 기기 실행에서 software lease 재발급은 검정 출력을 복구하지 못했고 DP 물리 분리/재연결만 복구함 | 최종 기기 증거 대기 | 각 프로필 강제 분리 후 Android 내부 화면이 부드럽고 토큰 소유 프로세스/DRM FD가 남지 않는지 확인하고, 신뢰 가능한 software retrain을 입증하거나 물리 재연결을 필수 복구 절차로 문서화 |
| V-19 | Vulkan 우선 preload 격리 | Staged runtime v12가 실제 `libdrm.so.2`에서 property-blob symbol을 찾고, 과거 `dlsym` interposer를 재현한 호스트 fixture도 통과. 기준 기기에서는 video `auto`로 gateway가 동작하는 동안 수정하지 않은 Sway/wlroots가 117초 동안 `WLR_RENDERER=vulkan` 유지, 컴포지터 preload에 Firefox 어댑터 없음, GLES2 재시도 없음, 종료 뒤 gateway/session 고아 없음 | 소스/staged 기기 통과, 최종 설치 ZIP 증거는 아님 | release 후보 설치와 공개 재부팅 절차 후 Sway 시작/종료 반복, Firefox 재생/seek 합격은 별도로 완료 |

V-07부터 V-09는 폐기한 ID입니다. 과거 Wayfire, River, Newm 후보는 v3 릴리스
프로필이 아닙니다.

## 실질적으로 남은 작업

하드웨어 Vulkan, QCOM/UBWC 우선, 일반 전체화면 직접 스캔아웃, CPU pixel 복사
없는 격리 코덱 경로는 구현했고 기준 기기의 제한된 증거도 확보했습니다. 실제
Firefox preload 어댑터를 프로세스 범위로 제한한 뒤에는 codec gateway를 켜도
Vulkan 우선 시작이 안정적으로 유지됩니다. 릴리스에 남은 작업은 다음 네 가지로
축약됩니다.

1. 정확한 릴리스 태그 패키지를 빌드·설치하고 세 프로필의
   시작/중지/강제분리/재연결을 반복
2. 해당 패키지에서 화면 player/browser 재생·seek와 경량 도구 및 OBS 인코드
   검증
3. USB 독 입력 소유권 매트릭스와 장시간 suspend/hotplug 누수 시험 완료
4. EGL 폴백 및 추가 Qualcomm 기기를 검증한 뒤 기준 기기 전용이 아닌 이식성
   주장

선택형 Waybar 전체화면 watcher는 bar가 보이는 합성 경로의 최적화이며, 이미
입증된 일반 전체화면 직접 스캔아웃의 blocker가 아닙니다. 냉각된 1.2 GHz와 열
제한 607 MHz의 vkmark 재측정도 성능 기준선 마감 항목이지 구현 선행 조건은
아닙니다.

## 갱신 규칙

Pending 행을 완료할 때 다음을 기록합니다.

1. 정확한 Git commit과 모듈 ZIP SHA-256
2. 빌드 flavor
3. 기기 모델과 고유하지 않은 소프트웨어 식별자
4. 범위가 제한된 테스트 입력과 합격 기준
5. 실패와 폴백을 포함한 측정 결과
6. 패키지 모듈 증거인지 격리 개발 프로브 증거인지 여부

기기 serial, 인증정보, 전체 Android 로그, 로컬 절대 경로를 공개하지 않습니다.
격리 프로브 증거는 그 사실을 계속 표시해야 합니다. 위 V-13부터 V-15까지는
기준 기기 경로 증거이며, 정확한 tag release 산출물 기록은 별도 완료 항목입니다.
