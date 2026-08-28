# LinDeX 아키텍처

[English](../ARCHITECTURE.md) | [한국어](ARCHITECTURE.md)

## 시스템 경계

LinDeX는 Android 디스플레이 소유권, Debian 렌더링, 물리 DP 스캔아웃을
분리합니다. 모듈은 Android `/system`, `/vendor`, `/product`를 다시 마운트하거나
쓰지 않습니다. 모듈 영구 상태는 `/data/adb/debian-drm-lease-kit` 아래에,
Debian chroot는 `/data/local/debian`에 있습니다.

```text
Android 디스플레이 소유자
  -> 범위가 제한된 DRM lease broker
  -> 상속된 lease 파일 디스크립터
  -> Debian libseat/DRM 브리지
  -> 수정하지 않은 Wayland 컴포지터
  -> MSM/SDE 물리 DisplayPort 출력
```

Qualcomm Android 기기는 렌더링을 KGSL(`/dev/kgsl-3d0`)로, 디스플레이
스캔아웃을 MSM/SDE DRM(`/dev/dri/card0`)으로 노출합니다. LinDeX는 두 역할을
분리하고 KGSL을 일반 DRM render node처럼 가장하지 않습니다.

## 네이티브 렌더러 파이프라인

렌더러와 디스플레이 컨트롤러는 서로 다른 역할을 담당합니다.

| 계층 | Vulkan 경로 | OpenGL/GLES 폴백 | 디스플레이 역할 |
|---|---|---|---|
| Debian userspace | Mesa Turnip | Mesa Freedreno/Gallium `kgsl` | 수정하지 않은 Sway/wlroots 또는 labwc가 allocation, 합성, scene 정책 소유 |
| Android stock kernel ABI | `/dev/kgsl-3d0`을 통한 Adreno 명령 제출 | `/dev/kgsl-3d0`을 통한 Adreno 명령 제출 | `/dev/dri/card0`으로 MSM/SDE KMS 노출, `/dev/dri/renderD128`은 DRM allocation/identity 매칭에 참여 |
| 기기 간 전달 | dma-buf object, 명시적 DRM format/modifier, native fence | dma-buf object, 명시적 DRM format/modifier, native fence | Lease된 connector, CRTC, primary plane이 물리 DP sink 구동 |

KGSL은 root 기기를 포함해 지원 대상 Android stock kernel에서 정상적인 Qualcomm
GPU 명령 제출 ABI입니다. Root는 이 ABI와 lease된 디스플레이 자원에 제한적으로
접근하게 해줄 뿐, 숨겨진 더 빠른 render node를 만들지 않습니다. 일반 upstream
Mesa `msm` 경로를 사용하려면 kernel이 그에 대응하는 일반 Adreno DRM render-node
ABI를 실제로 노출해야 합니다. `/dev/dri`가 존재한다는 이유만으로 이를 선택하면
MSM/SDE 디스플레이 쪽과 KGSL 렌더러를 혼동하게 되며 유효한 최적화가 아닙니다.

Stock 세션은 깨끗한 렌더러 환경에서 시작한 뒤 live hardware probe를 수행합니다.
Software가 아닌 Freedreno/Turnip ICD를 받아들이고
`FD_KGSL_ENABLE_DMABUF=1`을 활성화하며, 정확한 Vulkan/DRM identity gate도
통과할 때만 `WLR_RENDERER=vulkan`을 선택합니다. 자동 모드에서 Vulkan을 사용할
수 없거나 초기에 실패하면 hardware `WLR_RENDERER=gles2`와
`MESA_LOADER_DRIVER_OVERRIDE=kgsl`로 제한된 재시도 1회를 수행합니다. Vulkan
강제 정책은 fail-closed 처리하며 llvmpipe, lavapipe, SwiftShader 등의 software
renderer는 hardware probe를 통과하지 못합니다.

이는 네이티브 GPU 가속이지만 모든 프레임의 직접 스캔아웃을 뜻하지는 않습니다.
KGSL이 합성 프레임 전체를 GPU로 렌더링하면서 MSM/SDE는 컴포지터의 LINEAR
swapchain을 스캔아웃할 수 있습니다. 직접 스캔아웃은 하나의 적격 client
dma-buf를 leased primary plane에 바로 배치할 수 있는 더 좁은 경우입니다. UBWC도
다른 픽셀 형식이나 GPU API가 아니라 메모리 배치 modifier입니다. 데스크톱
`XB24`/`XR24`, 영상 `NV12`, QCOM/UBWC와 LINEAR는 buffer contract의 서로 다른
부분을 설명합니다.

## 디스플레이 브리지

브리지는 소유한 컴포지터 세션에만 로드됩니다. 해당 세션이 일치하는 KMS
기기를 열 때 이미 권한이 부여된 lease 디스크립터를 반환합니다. 다른 기기
열기는 평소대로 동작합니다. 컴포지터는 렌더러, allocator, scene graph, 직접
스캔아웃 판단을 계속 소유합니다.

기본 정책은 컴포지터의 modifier 협상을 보존합니다. 좁은 범위의 frontend는
기존 형식과 DRM LINEAR를 유지하면서 lease로 검증된 정확한 QCOM 후보만
leased primary plane의 `IN_FORMATS` 데이터에 추가할 수 있습니다. 화면 덮기,
겹침, 변환, 색 관리, 커서, 동기화 검사를 우회하지 않습니다. WebUI는 직접
스캔아웃에 `auto`와 호환용 `off`를 제공하며 위험한 강제 활성화 모드는 없습니다.

새 설치는 exact-device gate가 있는 `UBWC 우선`을 기본으로 선택하며 장치 검증
실패 시 원래 KMS 데이터를 그대로 둡니다. 두 `auto` 설정의 범위는 다릅니다.
출력 modifier `auto`는 호환성 우선 후보
추가이며 형식 우선순위를 뜻하지 않습니다. 직접 스캔아웃 `auto`는 장면 적격성
판정을 wlroots에 맡깁니다. 기준 기기의 원본 wlroots 0.18.2는 합성 출력 형식을
XR24로 초기화합니다. Waybar를 제거하고 불투명 전체화면 vkmark만 실행해도 출력
modifier `auto`에서는 90/90 XR24 LINEAR였습니다. 정확한 UBWC 우선 정책에서는
같은 시험이 LINEAR에서 88회의 XB24/QCOM으로 자동 전환됐고 Waybar 복원 뒤 다시
LINEAR로 돌아왔습니다. 즉 앱과 plane이 XB24/QCOM에 합의하면 직접 스캔아웃은
자동 동작하지만 후보 노출만으로 stock wlroots의 우선 선택은 바뀌지 않습니다.
합성 데스크톱 UBWC는 이 선택이 libdrm `IN_FORMATS`보다 위 계층에 있으므로
버전별 wlroots ABI 어댑터 또는 소스 패치가 필요합니다.

Waybar가 modifier 협상을 끄는 것은 아닙니다. Archcraft bar가 top layer에
매핑되면 출력 scene에는 전체화면 클라이언트와 두 번째 layer-shell buffer가
동시에 존재합니다. 원본 wlroots 0.18과 0.20은 적격 scene buffer가 하나일 때만
직접 스캔아웃 경로에 들어가므로, 이 경우 스캔아웃 시험 자체를 생략하고 Sway의
XR24 LINEAR 합성 swapchain을 사용합니다. 알림, 런처, 팝업, software cursor도
같은 일시적 폴백을 만들 수 있습니다. 추가 scene 항목이 사라지면 XB24/QCOM으로
자동 복귀합니다. Waybar를 표시한 채 직접 스캔아웃을 강제하면 bar가 사라질
뿐이므로 지원하지 않습니다.

향후 선택형 전체화면 성능 모드는 Waybar 0.12의 `SIGUSR1` 토글 대신 결정적인
Sway bar IPC를 사용해야 합니다. Sway가 이름이 지정된 bar 하나를 소유하고,
Waybar는 같은 ID와 `ipc: true`로 구독하며, 범위가 제한된 Sway event watcher가
실제 fullscreen container에서만 bar를 `invisible`로 바꾸고 종료 뒤 `dock`으로
복원해야 합니다. IPC 재연결 뒤에도 상태를 복원하고, 동일 lease GETFB2 A/B에서
LINEAR에서 QCOM으로 전환되는 증거가 확인되기 전에는 성공으로 광고하지
않습니다. 항상 표시되는 합성 bar는 별도의 버전 제한 컴포지터 어댑터가 없으면
LINEAR로 유지됩니다.

모든 bridge와 preload-only 경로는 live identity 및 capability 검사로 범위를
제한하며 gate가 누락되면 fail-closed로 동작합니다. Lease나 modifier bridge의
성공만으로 직접 스캔아웃을 증명하지는 않습니다. 수정하지 않은 Wayland
컴포지터가 전체 scene 및 scanout 판정을 계속 담당합니다.

LinDeX는 고정된 KGSL 지원 Mesa 빌드를 사용합니다. Vulkan은 live probe가
성공할 때만 우선하며 GLES는 폴백으로 남습니다. 직접 스캔아웃 결과에는 여전히
앱 버퍼, 렌더러, leased plane의 형식과 modifier가 일치해야 합니다.

정확히 검증된 Turnip 스택에서는 표준 explicit layer 기반
[Vulkan DRM identity 브리지](VULKAN_DRM_IDENTITY.md)가 수정하지 않은 wlroots가
KGSL 렌더러와 MSM/SDE KMS를 연결하는 데 필요한 누락된
`VK_EXT_physical_device_drm` metadata를 제공합니다. 정확한 driver, capability,
node, 실행 시 측정한 rdev gate는 DRM `IN_FORMATS` modifier 브리지와 독립적입니다.

Preload 범위도 렌더러 안전 경계입니다. 컴포지터에는 디스플레이, seat, 선택적
USB 입력 브리지 라이브러리만 전달합니다. Firefox 전용 EGL/RDD 어댑터는
패키지된 브라우저 실행기가 주입하며 Sway 또는 labwc 프로세스에는 들어가지
않습니다. 런타임 v12는 두 property-blob 진입점을 로드된 실제
`libdrm.so.2` 객체에서 찾고 자기 자신 또는 interposer로 돌아오는 결과를
거부합니다. 따라서 관련 없는 `dlsym` 어댑터가 DRM 훅을 재귀 호출로 바꿔
컴포지터를 종료시키고 GLES 폴백으로 오인하게 만드는 일을 막습니다.

## 세션 수명주기

모든 시작에는 고유 세션 식별자, leader PID, 프로세스 그룹, 세션 소유 토큰,
lease가 배정됩니다. 정리는 토큰을 가진 전체 그룹을 검사하므로 launcher leader가
먼저 종료돼도 DRM FD를 가진 컴포지터 자식이 고아로 남지 않습니다. 정상 중지,
컴포지터 실패, 물리 DP 분리는 소유한 그룹만 종료하고 디스크립터를 닫습니다.
Connector가 명시적으로 `disconnected`이면 첫 watcher sample에서 중지하며,
연결 상태지만 EDID만 일시적으로 읽히지 않을 때만 2회 debounce를 사용합니다.
재연결은 유효한 DP+EDID가 연속 2회 확인되어야 하며 새 lease, 토큰, 프로세스
그룹을 만들고 실패한 lease는 재사용하지 않습니다.

자동 시작에는 물리적으로 연결된 DP connector와 유효한 EDID 데이터가 모두
필요합니다. 사용자가 실험적 수동 모드를 명시적으로 고르지 않으면 연결된
모니터가 제공하는 모드를 사용합니다.

## Debian 및 프로필 설정

Android 모듈은 WebUI 명령, rootfs 추출, 기기 검색, 세션 제어, 선택적
MediaCodec 서비스를 소유합니다. Debian chroot는 컴포지터 패키지, 사용자
설정, Mesa, 프로필 런타임을 소유합니다.

3개 릴리스 프로필은 공통 기본 의존성을 공유합니다. root chroot에는 인증
에이전트가 사용할 수 있는 logind 사용자 세션이 없으므로 LinDeX는 `lxpolkit`,
`xfce-polkit`, `lxqt-policykit`을 추가하지 않습니다. 따라서 LXQt는 `lxqt-core`
대신 명시적 구성요소로 설치합니다. Archcraft Sway Free는 문서화된 Archcraft
GitHub commit의 공식 공개
GPL-3.0 Sway dotfile·배경화면과 Dark/Light GTK 테마·아이콘·커서를 고정하고
checksum을 검증해 설치합니다. 외형 archive는 LinDeX가 만든 재현 가능 aggregate며
Archcraft 업스트림 배포물이 아닙니다. 프로필 관리자는 시작 전에 Dark, Light 또는
공식 `~/.config/sway/theme/theme.sh --pywal` 경로를 적용합니다. 좁은
Debian/chroot 어댑터는 Android가 소유하는 전원, 내부 밝기, Bluetooth 동작과
자동 잠금/DPMS를 비활성화합니다. LXQt와 XFCE는 stock labwc를 사용합니다.
Wayfire, River, Newm, private provider stack, Ko-fi archive는 현재 릴리스 경계
밖입니다.

## 비디오 브리지

선택적 ADVC 서비스는 디스플레이 lease와 독립적입니다. backend 선택은 코덱
세션마다 한 번 수행됩니다.

```text
Vulkan dma-buf -> EGL dma-buf -> Android 로컬 AHB/Surface -> byte 전송
```

엄격한 zero-copy 모드는 live 검증된 dma-buf backend만 허용하며 CPU raw-pixel
복사로 폴백하지 않습니다. Auto 모드는 Android 로컬 하드웨어 또는 byte 전송을
사용할 수 있습니다. backend가 실패하면 새 세션을 만들기 전에 소유한 버퍼,
디스크립터, fence를 정리하며 프레임 도중 전환하지 않습니다.

인코드 후보는 기준 기기에서 CPU raw-pixel 복사 0회와 MediaCodec Surface로의
제한된 GPU blit 1회를 증명했습니다. 디코드 장기 실행은 QCOM/UBWC→명시적
LINEAR Vulkan repack으로 H.264 Main+B 180/180, HEVC Main+B 120/120, VP9 Profile
0 120/120을 CPU raw-pixel 복사 없이 완주했습니다. 설치된 v3.0.3-dev Firefox RDD
경로도 샌드박스를 유지하고 RDD KGSL descriptor 없이 `c2.qti.avc.decoder`를 통해
seek 30회와 document reload 10회를 완료했습니다. HEVC Main과 VP9 Profile 0은
각 정확한 live preflight 뒤 공개 베타로 광고하며, 화면 앱 호환성과 공개
tagged-release 반복은 더 확대해야 합니다. Modifier 값 `0`은 디스크립터가
DRM LINEAR라고 명시한 경우에만 유효하며 누락된 메타데이터로 취급하지 않습니다.

공개 주장 범위와 남은 릴리스 게이트는
[코덱 및 zero-copy 상태](VIDEO_ZERO_COPY_STATUS.md)를 확인하세요.

## 입력 라우팅과 로그

USB 키보드, 마우스, 간접 터치패드 소유권을 활성 컴포지터 세션으로 제한할 수
있습니다. 휴대폰 터치스크린과 관련 없는 USB 기능은 Android에 남습니다. 마지막
소유 디스크립터가 닫히면 grab이 해제됩니다.

release 패키지는 설정, 세션, 코덱 진단 스트림을 `/dev/null`로 보내고 오래된
영구 로그를 제거합니다. dev 패키지는 크기가 제한되고 순환되는 진단 로그만
보존합니다. [USB 입력 라우팅](USB_INPUT_ROUTING.md)과
[안전](SAFETY.md)을 확인하세요.
