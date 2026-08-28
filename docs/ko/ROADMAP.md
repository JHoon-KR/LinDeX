# 로드맵

[English](../ROADMAP.md) | [한국어](ROADMAP.md)

## 짧은 마감 목록

1. 최종 태그 ZIP과 세 프로필 lifecycle/hotplug 매트릭스
2. 패키지 player/browser decode-seek 및 경량 도구/OBS encode 증거
3. USB 독 입력 소유권과 장시간 suspend/hotplug 누수 시험
4. EGL 폴백과 추가 기기 이식성 증거

기준 기기의 핵심 그래픽 작업인 하드웨어 Vulkan, QCOM/UBWC 우선 및 일반
전체화면 직접 스캔아웃은 더 이상 미구현 항목이 아닙니다.

## Version 3 공개 릴리스 게이트

- [x] LinDeX 이름 아래 av2xn 프로젝트 계보와 MIT 고지를 보존.
- [x] 수정하지 않은 컴포지터 런타임과 작은 DRM lease 브리지를 사용.
- [x] Android `/system`, `/vendor`, `/product`를 쓰기 경로 밖에 유지.
- [x] Rootfs 포함 KernelSU 설치 경로와 WebUI 제공.
- [x] Release/no-persistent-log 및 dev/bounded-log 패키지 분리.
- [x] 프로세스, lease, 분리, USB 입력 정리를 소유 세션으로 제한.
- [x] 지원하는 3개 프로필인 Archcraft Sway Free, LXQt + stock labwc,
  XFCE + stock labwc 제공.
- [x] chroot PolicyKit 에이전트를 제외하고 LXQt는 명시적 구성요소로 풀어
  `lxqt-policykit` 없이 PCManFM-Qt를 유지.
- [x] Archcraft 공식 공개 GitHub 저장소의 checksum 고정 GPL-3.0 Sway
  자산을 패키징하고 비공개/유료 archive는 소스와 릴리스 밖에 유지.
- [x] 고정된 공개 자산 aggregate와 공식 theme script를 사용해 WebUI 빠른
  시작에서 Sway 공식 Dark, 공식 Light, Pywal 모드 제공.
- [x] 지원되지 않는 엄격 코덱 경로와 디코드 PRIME을 fail-closed로 유지.
- [x] 영문/한국어 사용자 및 릴리스 대응 문서 공개.
- [ ] 정확한 태그에서 깨끗한 release 및 dev ZIP 검증.
- [ ] DP 분리 상태에서 정확한 release ZIP을 설치하고 Android 일반 재부팅.
- [ ] 3개 프로필 시작/중지/분리/재연결 기기 매트릭스 완료.
- [x] 기준 기기 패키지 모듈에서 제한된 FFmpeg/GStreamer H.264/HEVC 및 OBS
  H.264 texture 인코드 측정 완료.
- [x] 제한된 명시적 `modifier = 0` LINEAR 디코드/import 검증 완료.
- [ ] USB 독 매트릭스 완료.
- [x] 정확한 UBWC 우선 게이트에서 일반 전체화면 동일 lease 직접 스캔아웃 증거
  완료. 호환성 우선 `auto`는 LINEAR 유지.
- [ ] 선택형 ID 기반 Sway/Waybar 전체화면 watcher를 추가하고 결정적인
  `dock`/`invisible` 전환을 동일 lease GETFB2로 입증. Waybar 0.12의 비멱등
  `SIGUSR1` 토글을 쓰거나 표시 중인 bar 위로 스캔아웃을 강제하지 않음.

## 코덱 후속 작업

- [x] QCOM UBWC H.264 decoder PRIME 120 frame+EOS를 raw-pixel CPU 복사 없이
  Turnip content 및 release-fence 검증까지 통과.
- [x] 기준 decoder가 direct LINEAR buffer를 만들지 않았으므로 명시적
  `modifier = 0` one-copy LINEAR repack + Turnip 경로 검증.
- HEVC POC 기준 픽셀 정확도와 패키지 플레이어 화면 재생/seek를 완료한 뒤 해당
  확장 디코드 경로를 광고.
- 모호한 modifier 메타데이터를 거부하고 `0`은 명시된 DRM LINEAR로만 허용.
- 패키지 모듈에서 malformed descriptor, 연결 해제, teardown, 세션 재생성 테스트 반복.
- CPU raw-pixel 복사 0회와 GPU 작업 횟수를 별도로 공개.

## 신뢰성 게이트

Production-ready를 주장하기 전에 문서화된 한 기기 프로필이 장시간 데스크톱
사용, 반복 hotplug, Android 디스플레이 복구, suspend/resume을 lease, FD,
framebuffer, GPU object 누수 없이 완료해야 합니다. 정확한 시간과 cycle 수는
릴리스 증거와 함께 공개해야 합니다.

## 이식성

- Adreno 830 이외에 독립적으로 검증한 Qualcomm 기기 프로필 추가.
- KernelSU를 먼저 검증하고 Magisk/APatch는 별도 테스트 후에만 문서화.
- Debian/wlroots 업데이트 후 stock 컴포지터 동작 재실행.
- Vulkan 불가/EGL 가능 기기용 명시적 검증 추가.
