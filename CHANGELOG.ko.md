# 변경 기록

[English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md)

## 3.0.4 — 2026-08-28

- HEVC Main과 VP9 Profile 0 디코드를 공개 베타 광고로 승격했습니다. 각 profile은
  계속 fail-closed이며 현재 session이 정확한 QTI component, non-secure PRIME
  transport, 성공한 120/120 장기 실행 token을 확인한 경우에만 나타납니다.
- 외부에서 전달하는 앱 승인 token을 코덱 승격 조건에서 제거했습니다. 화면 표시,
  seek, EOS, sandbox, 종료 결과는 앱 호환성 증거로 추적하며 hardware profile을
  위조할 수 없습니다.
- `vainfo`, FFmpeg, mpv, GStreamer, Firefox ESR, OBS에서 목록·전송·화면 재생·정상
  종료를 구분해 검증하는 하드웨어 코덱 시험법을 영문·한글 문서에 추가했습니다.

## 3.0.3 — 2026-08-28

- H.264 B-frame 재정렬 출력이 도착하기 전에 dma-buf를 내보낼 수 있도록
  protocol 1.8 비동기 LINEAR 예약을 추가했습니다. 이후 완성된 QCOM/UBWC
  프레임은 CPU 픽셀 복사 없이 명시적 Vulkan fence로 같은 버퍼에 결합됩니다.
- 표시 높이 1080과 coded allocation 높이 1088 불일치, MediaCodec timestamp
  정규화, 예약 ID의 실제 output ID 승격, 프레임별 GPU 직렬화를 수정했습니다.
  기준 기기에서 H.264 B-frame 60/60, no-B 30/30, HEVC 디코드 및 H.264/HEVC
  인코드 smoke가 통과했습니다.
- 디코드 출력과 인코드 입력 modifier 정책을 보수적인 `auto|linear|qcom`으로
  분리했습니다. QCOM 직접 경로는 descriptor, broker, importer 검증을 모두
  요구하며, 그 외 `auto`는 CPU 픽셀 복사 없는 명시적 LINEAR GPU 폴백을
  사용합니다.
- 기본 profile session이 정확한 비동기 디코드 검증 gate를 내보내도록 했습니다.
  Protocol 1.8 코드는 포함됐지만 Firefox와 일반 VA-API 앱이 기존 동기 경로를
  선택하던 활성화 누락을 해소했습니다.
- 격리 환경의 H.264 4-frame 재정렬 상한을 실제 Firefox ESR RDD 제출 창에 맞는
  1-frame 상한으로 교체했습니다. 설치된 v3.0.3-dev production gateway에서
  seek 30회와 document reload 10회를 완료했고 RDD seccomp mode 2, RDD KGSL FD
  0개, `c2.qti.avc.decoder`, hardware output 1,757회, LINEAR export 1,758회,
  timeout 및 비동기 실패 0회, session 종료·재시작 고아 0을 확인했습니다.

## 3.0.2 — 2026-08-28

- QTI 디코드, QCOM PRIME, Vulkan repack, 명시적 LINEAR/fence, EOS 및 종료
  행렬이 통과했는데도 운영 Android broker의 `ADVC_FEATURE_DECODE_PRIME`을
  계속 닫던 시작 게이트 누락을 수정했습니다.
- 오래된 검증 전용 토큰을 정확한 버전형 release 토큰으로 교체하고 KernelSU
  broker 서비스가 외부에서 상속된 임의 값을 덮어쓰도록 했습니다.
- release 영구 로그를 활성화하지 않으면서 협상된 transport, ready, hardware,
  advertised 코덱 mask를 확인할 수 있는 VA-API 정책 추적을 추가했습니다.
- 정확한 Firefox RDD 역할에서만 root 소유 ADVC VA 드라이버를 RDD seccomp
  설치 전에 미리 로드합니다. 샌드박스는 유지되고 RDD에 KGSL 권한을 주지
  않으며, 이후 libva 초기화는 이미 매핑된 드라이버를 재사용합니다.

## 3.0.1 — 2026-08-28

- 등록 dma-buf와 API 31 이상 AHardwareBuffer의 EGL 인코드 경로에서 프레임별
  resource 생성과 `glFinish()` 직렬화를 제거했습니다. 정체성을 검증하는 제한된
  cache가 import image/texture를 보유하고 native fence를 전달하며, 구형 또는
  확장 기능이 부족한 기기는 기존 동기식 안전 경로를 유지합니다.
- HEVC Main과 VP9 Profile 0용 제한 시간 capability preflight를 패키지에
  추가했습니다. 두 profile은 기본 미광고이며, 실시간 hardware/PRIME transport
  결과와 별도의 기기별 화면 재생·seek 검증 ACK가 모두 있어야만 열립니다.
- Firefox RDD repack 경로에 5초 protocol watchdog, stale/재사용 FD 정체성 검사,
  오류 연결 폐기, stale socket 복구, listener backlog 포화까지 제한하는
  nonblocking AF_UNIX connect를 추가했습니다.
- 128회 lifecycle, 32회 강제 disconnect, backlog 포화, stale socket, FD 기준선,
  timeout, sanitizer, 정확한 앱 gate 회귀 검사를 추가했습니다. Firefox sandbox와
  선연결 소켓 경계는 그대로 유지합니다.

## 3.0.0 — 2026-08-28

- Vulkan 인코드 producer의 프레임별 import 및 전체 queue idle 병목을
  제거했습니다. 등록 dma-buf는 정체성을 검증한 제한된 import cache, 4개
  in-flight command/fence slot, swapchain image별 present semaphore를 사용합니다.
  명시적 acquire/release sync-file 소유권과 fail-closed teardown은 유지합니다.
  Android ARM64 NDK 빌드와 호스트 회귀를 통과했으며 EGL 폴백은 별도 fence-safe
  수명주기 구현 전까지 의도적으로 동기식 상태를 유지합니다.
- 영속 인코더 종료에 명시적 EOS 마무리를 추가했습니다. EOS를 정확히 한 번
  보내고 단일 5초 예산 안에 drain한 뒤 surface unregister와 broker close를
  수행합니다. Signal, timeout, protocol, stale-registration 실패는 fail-closed이며
  호스트·sanitizer 테스트와 재현 가능한 ARM64 VA driver 빌드를 통과했습니다.
- 격리 코덱 매트릭스를 확대했습니다. QCOM/UBWC→LINEAR Vulkan repack에서
  CPU raw-pixel 복사 없이 H.264 Main+B 180/180 147.28fps, HEVC Main+B 120/120
  118.73fps, VP9 Profile 0 120/120 108.46fps를 완주했습니다. H.264에는 검증된
  4-frame reorder gate만 적용하고 미지원 profile은 계속 숨깁니다.
- 720p60 인코드에서 H.264 CB 84.9fps, HEVC Main 81.1fps를 기록했습니다.
  PRIME/OBS 입력은 CPU raw-pixel 복사 0회와 GPU 변환 1회, 일반 FFmpeg upload는
  CPU 복사 1회와 GPU 변환 1회입니다.
- 패키지 이름과 모듈 버전을 `VERSION`에서 생성하도록 하고, 여러 스크립트에
  중복되어 오래될 수 있던 hash·byte count 대신 검토된 ADVC checksum manifest
  하나를 신뢰 기준으로 사용하도록 릴리스 검증을 정리했습니다.

- Firefox 전용 `dlsym` 어댑터를 컴포지터 전체에 로드해 발생한 Vulkan 우선
  시작 회귀를 수정했습니다. Firefox EGL/RDD 어댑터는 패키지된
  `lindex-firefox` 실행기를 통해서만 활성화하며 Sway, labwc 및 데스크톱
  자식에는 디스플레이·입력 브리지 preload만 전달합니다.
- 불변 디스플레이 브리지 런타임을 v12로 올렸습니다. DRM frontend는 실제
  `libdrm.so.2` link map에서 `drmModeGetPropertyBlob`과
  `drmModeFreePropertyBlob`을 찾고, 주소가 preload wrapper 자신으로 돌아오면
  fail-closed 처리합니다. 과거 `dlsym` interposer를 재현하는 호스트 회귀
  fixture를 추가했고, 기준 기기에서는 codec gateway를 켠 상태에서도 수정하지
  않은 Sway/wlroots가 GLES2 재시도 없이 `WLR_RENDERER=vulkan`을 유지했습니다.
- Archcraft Sway 네트워크 위젯을 완전한 읽기 전용으로 변경했습니다. 프로필은
  더 이상 `nm-applet`을 시작하지 않고 Waybar의 `rofi_network` 우클릭 동작도
  제거하여 Android만 Wi-Fi 정책을 소유합니다. 이 정책과 Qualcomm CNSS/HAL
  복구 고착을 구분하는 진단 기준도 추가했습니다.
- Fail-closed Firefox RDD gateway 후보 추가. RDD 샌드박스와 선연결 소켓을
  유지하고, root 전용 helper가 검증된 QCOM/UBWC→LINEAR Turnip repack과
  명시적 fence를 담당합니다. RDD용 VA 드라이버에는 Vulkan 의존성이 없습니다.
- Android ADVC broker를 client별 동시 worker 구조로 재빌드하고 repack gateway에
  parent-death 정리를 추가해 강제 세션 종료 뒤 고아가 남지 않게 했습니다.
- Android toybox `tr`가 `/proc` cmdline/environ descriptor를 직접 잡지 않도록
  수정했습니다. 제한된 `dd` snapshot을 닫은 뒤 변환하므로 이전에 관측된
  PID 1 소유 고CPU 고아 경쟁을 예방합니다.
- Archcraft 공개 타 배포판 설치 목록과 맞도록 Sway 의존성 계약에 `wofi`,
  `kanshi`, `wlogout`을 복구했습니다. root chroot에서 불필요한 PolicyKit
  에이전트는 계속 제외합니다.
- `hyprpicker`와 `hyprlock` 준비 판정이 실제 dpkg 패키지를 요구하도록 바꾸고,
  공개 전 호환 래퍼는 정확한 해시가 일치할 때만 제거하도록 이행을 추가했습니다.
- 설치된 FontAwesome family로 Archcraft Waybar 테마 glyph 깨짐을 수정하고,
  Waybar 0.12의 삼성 `charge_now=0` 계산을 읽기 전용 표준 capacity module로
  우회했습니다. 향후 오디오 서버용 PulseAudio 위젯은 유지합니다.
- av2xn/DOAN과 av2xn/Magisk-Debian-Chroot의 MIT 고지와 계보를 보존하면서
  JHoon이 관리하는 공개 프로젝트 이름을 LinDeX로 변경.
- 패키지된 과거 컴포지터별 런타임 방향을 수정하지 않은 컴포지터용 작은
  immutable revision DRM lease 브리지로 교체.
- 검증된 Qualcomm Turnip이 수정하지 않은 wlroots에서 정확한 KMS node와
  일치하도록 fail-closed 표준 Vulkan DRM identity layer 추가. Native 확장 정보는
  그대로 유지하고 GLES2 폴백도 보존.
- Android가 KGSL 문자장치 major를 동적으로 배정해도 수정하지 않은 wlroots의
  Vulkan 매칭이 성공하도록 수정. v2 세션 ACK는 고정 major 대신 정확한 세 장치
  경로를 실행 시 측정하고 재검증해 생성.
- 수정하지 않은 Sway/wlroots Vulkan 출력에서 1920×1080 전체화면 Mailbox
  vkmark 12,660점 확인. 직전 GLES2 폴백은 4,078점.
- 합성 Vulkan DRM identity를 compositor PID 하나로 제한하고 physical device
  group dispatch, 고정 table 포화 시 누수 없는 수명주기, 상속 lease FD의
  close-on-exec를 추가. 보강 경로에서도 Vulkan을 유지했고 제한된 전체화면
  vertex 2회 측정에서 19,927~20,726 FPS 확인.
- Android `/system` 오버레이 설치 제거.
- Rootfs 포함 새 설치 패키지 및 전체 데이터가 제거되는 uninstall 추가.
- 공개 설치 후 활성화 절차는 Android 일반 재부팅으로 명시. 소프트 리부트 기록은
  임시 루트 참조 기기에만 적용.
- Archcraft Sway Free와 Debian LXQt/XFCE + stock labwc의 원버튼 WebUI 설정 추가.
- DRM lease를 소유한 labwc가 XFCE의 stock labwc 설정과 정상 `xinitrc`
  초기화를 사용하되 중첩 compositor는 만들지 않도록 XFCE Wayland 시작을 수정.
  단일 외부 Wayland 출력에서 패널, 바탕화면 아이콘, 독, 네이티브 터미널을
  실기 확인.
- Debian Firefox ESR 140.14의 headless Wayland 검증은 H.264 browser fixture를
  software FFmpeg 경로로 재생했으므로 Firefox hardware decode는 계속 fail-closed로
  유지. B-frame reorder/VUI 수정은 실제 DP/WebRender import, seek, drain,
  teardown을 통과할 때까지 소스 단계 후보로만 보존.
- 릴리스 범위를 위 3개 프로필로 축소. Wayfire, River, Newm은 대응하는 Archcraft
  공식 공개 dotfile 세트를 패키징하지 않으므로 제거.
- 공통 및 프로필 계약에서 `lxpolkit`/`xfce-polkit` 제거. LXQt는
  `lxqt-core`를 명시적 구성요소로 풀어 PCManFM-Qt 데스크톱/파일 관리자는
  유지하면서 `lxqt-policykit`도 제외. 업그레이드 설정은 이 구형 에이전트와
  메타패키지만 purge하고 유지할 데스크톱 구성요소에는 `autoremove`를 실행하지 않음.
- Release/no-persistent-log와 dev/bounded-log flavor 분리.
- 자식에게 상속되는 세션 토큰으로 hot-unplug 정리 보강. Launcher leader가 먼저
  종료돼도 토큰을 가진 컴포지터 자식을 정리하고, connector 명시적 분리는 첫
  sample에서 중지하며, 시작 보호는 Type-C partner가 남을 때만 허용하고 자동
  재연결 전에 DP+EDID 연속 2회 안정 상태를 요구.
- WebUI에서 시작한 세션을 compositor 실행 전에 Android 관리자 앱 freezer
  cgroup에서 분리.
- chroot에 BlueZ system bus가 없을 때 Waybar 전체를 종료시킬 수 있는 Bluetooth
  모듈과 Android가 소유하는 backlight 모듈을 Waybar가 생성하지 않도록 수정.
- 유지한 Waybar 배터리·네트워크 모듈을 Android의 표준 `battery` 전원 공급
  장치와 `wlan*` 인터페이스에 연결해 삼성 보조 전원 장치 및 정책 라우팅
  `dummy0` 선택으로 생기는 거짓 `0%`·`Disconnected` 표시 수정.
- 이전 홍보 갤러리 스크린샷과 나중에 공개된 설치 가능 `Type-2` Waybar 설정이
  서로 다른 업스트림 revision이라는 점을 문서화.
- 활성 모니터 세션 범위의 USB 키보드/마우스/터치패드 전용 라우팅 추가.
- EDID 기반 모드와 명시적 실험 100/120/144Hz 선택 추가.
- 하드웨어 비디오 backend 선택과 CPU raw-pixel 복사 0회, MediaCodec Surface로
  GPU blit 1회의 Vulkan dma-buf 인코드 후보 검증 추가.
- 지원되지 않는 코덱 경로와 디코드 PRIME 내보내기를 정확한 패키지 모듈
  import/content/modifier/fence 검증 전까지 fail-closed로 유지.
- 기준 기기에서 H.264 QCOM UBWC 120 frame+EOS의 Turnip content/release-fence
  검증과 명시적 modifier-0 LINEAR Vulkan repack 120 frame을 통과. 프레임당
  GPU 복사 1회, CPU raw-pixel 복사 0회.
- dma-buf descriptor cleanup이 미선언 0 값 object slot을 닫아 GStreamer의
  FD 0 bus/poll wakeup descriptor를 파괴하던 문제 수정.
- Archcraft 공식 공개 GPL-3.0 Sway dotfile·배경화면(`archcraft-sway` commit
  `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`)과 공식 공개 GPL-3.0
  Dark/Light 테마·아이콘·커서 소스의 checksum 고정 사본을 포함. 외형 archive는
  고정된 공개 파일을 LinDeX가 재현 가능하게 묶은 aggregate이며 Archcraft 업스트림
  배포 archive 또는 Ko-fi archive가 아님을 명시.
- 고정 저장소의 실제 기본 배경은 바다·바위 사진이며 Archcraft 갤러리의 어두운
  꽃 배경은 합성 스크린샷 안에만 있고 재배포 가능한 원본 자산으로 취급하지
  않음을 문서화.
- 설치된 공식 Sway script `~/.config/sway/theme/theme.sh --pywal`을 통한 공식
  Dark, 공식 Light, Pywal 생성 선택을 WebUI 빠른 시작에 추가.
- Archcraft 시각 파일은 유지하면서 Android가 소유하는 전원, 내부 밝기,
  Bluetooth, 자동 잠금/DPMS 동작만 비활성화하는 좁은 Debian/chroot 어댑터 추가.
- 3개 릴리스 프로필 manifest와 설치기를 동기화하고 공개 앱 목록과 내부 전이
  런타임 라이브러리를 분리했으며, 패키지·실행 명령·Sway 자산 검증 후 ready 판정.
- WebUI가 패키지된 프로필 metadata에서 목록을 갱신하고, 내장 목록은 오프라인
  폴백으로 유지하도록 변경.
- 공개 사용자 및 릴리스 문서를 영문/한국어 대응 페이지로 분리.

과거 v2 컴포지터 실험은 로컬 ignore된 `legacy/`와 `patches/` 아래에 남으며
공개 v3 소스 범위에는 포함되지 않습니다.
