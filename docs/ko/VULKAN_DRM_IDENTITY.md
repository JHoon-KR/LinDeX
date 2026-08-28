# Vulkan DRM identity 브리지

[English](../VULKAN_DRM_IDENTITY.md) | [한국어](VULKAN_DRM_IDENTITY.md)

검증된 Android KGSL 기준 스택의 Turnip은 실제 하드웨어 Vulkan 렌더러이지만
Mesa 26.2-devel은 `VK_EXT_physical_device_drm`을 광고하지 않습니다. 따라서
수정하지 않은 wlroots 0.18과 0.20은 Vulkan 물리 장치와 lease KMS 장치를
연결하지 못하고 `Could not match drm and vulkan device`를 출력합니다.

`libandroid-vulkan-drm-identity-layer.so`는 범위를 좁힌 표준 Vulkan explicit
layer이며 wlroots, Sway, Mesa 패치가 아닙니다. Stock 세션이
`VK_LAYER_PATH`와 `VK_INSTANCE_LAYERS`로 활성화하고 Vulkan loader dispatch를
통해 장치 확장 열거와 core/KHR `vkGetPhysicalDeviceProperties2`를 중간
처리합니다.
드라이버가 이미 `VK_EXT_physical_device_drm`을 제공하면 native 결과를 추가나
교체 없이 그대로 전달합니다.

## Fail-closed gate

합성 identity는 세션이 소유한 다음 세 값이 정확히 일치할 때만 활성화됩니다.

```text
ANDROID_VULKAN_DRM_IDENTITY_ENABLE=1
ANDROID_VULKAN_DRM_IDENTITY_ACK=turnip-qualcomm-card0-renderD128-kgsl3d0-<primary-major>-<primary-minor>-<render-major>-<render-minor>-<kgsl-major>-<kgsl-minor>-v2
ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID=<compositor-pid>
```

ACK만으로는 충분하지 않습니다. owner PID가 호출 프로세스 PID와 같아야 하므로
데스크톱 앱이 layer 환경을 상속해도 합성 identity는 compositor 한 프로세스에만
적용됩니다. 각 Vulkan 물리 장치에 대해 Qualcomm vendor
ID `0x5143`, Mesa Turnip driver ID/name, external-memory fd/dma-buf와 DRM format
modifier 확장, 그리고 고정된 세 경로 `/dev/dri/card0`,
`/dev/dri/renderD128`, `/dev/kgsl-3d0`의 실제 비-symlink 문자 장치가 v2
ACK에 담긴 live `st_rdev` 값과 정확히 일치하는지 모두 확인합니다.

전부 일치할 때만 `VK_EXT_physical_device_drm`을 추가하고 측정한 primary와
render 값을 채웁니다. Android는 부팅 후 KGSL 문자장치 major를 동적으로
배정할 수 있으므로 이전 기준 기기의 `466:0`을 가정하지 않습니다. Callback,
확장, 노드, 정확한 rdev, vendor, driver, 메모리 할당 또는 ACK 중 하나라도
실패하면 원래 Vulkan 결과를 반환합니다. 임의 장치를 검색하거나 추측하지
않습니다.

## 런타임 동작

stock profile 세션은 `vulkaninfo`가 software가 아닌 Freedreno/Turnip ICD를
선택한 뒤에만 layer를 추가합니다. Vulkan을 사용할 수 없으면 기존처럼
하드웨어 GLES2를 선택합니다. 자동 모드의 초기 Vulkan compositor 실패 시
layer와 ACK를 제거한 다음 GLES2를 한 번 재시도합니다. 이 metadata
브리지는 Vulkan 렌더링, dma-buf import, modifier, direct scanout 또는 출력
성공을 주장하지 않으며 이들은 계속 live runtime 판단입니다.

`scripts/test-vulkan-drm-identity.sh`는 fake callback으로 정확한 opt-in과 owner
PID, 모든 거부 gate, Vulkan count/`VK_INCOMPLETE` 규칙, 동적 rdev 파싱,
core/KHR properties, physical-device-group dispatch, native 확장 pass-through,
고정 크기 table 포화 시 cleanup과 slot 재사용, loader dispatch, versioned layer
ABI를 검사합니다.

2026-08-27 기준 기기 실측은 `card0=226:0`, `renderD128=226:128`, 동적
`kgsl-3d0=462:0`이었습니다. 수정하지 않은 Sway/wlroots가
`WLR_RENDERER=vulkan`을 유지했고 1920×1080 전체화면 Mailbox vkmark는
**12,660점**으로, 직전 GLES2 경로 **4,078점** 대비 약 3.1배였습니다.
수명주기와 자식 범위 보강 뒤에도 Vulkan이 유지됐고, 제한된 전체화면 Mailbox
vertex 2회 측정은 **19,927~20,726 FPS**였습니다. Sway 자식 프로세스는 열린 lease DRM FD를
상속하지 않았고 owner 값도 자기 PID와 달라 layer는 원본 pass-through로
동작했습니다.

## Firefox preload 회귀와 v12 수정

이후 발생한 Vulkan 우선 실패는 identity layer 또는 Turnip probe 실패가
아니었습니다. 코덱 설정이 Firefox 전용 어댑터 두 개를 세션 전체
`LD_PRELOAD`에 추가하고 있었습니다. 먼저 로드된 EGL 어댑터가 `dlsym`을
가로채므로 DRM preload가 다음 `drmModeGetPropertyBlob` 구현을 찾을 때 DRM
wrapper 자기 자신을 다시 받을 수 있었습니다. 컴포지터가 `SIGSEGV`로 종료된
뒤 정상적인 1회 복구가 GLES2를 선택해 Vulkan 선택 문제처럼 보였습니다.

런타임 v12는 서로 독립적인 두 수정을 적용합니다.

1. Firefox 어댑터를 컴포지터 환경에서 제거하고 검증된 `lindex-firefox`
   실행기를 통해서만 주입합니다.
2. DRM preload가 실제 `libdrm.so.2` link-map 객체를 열어 두 property-blob
   함수를 그 객체에서 찾고 `dladdr`로 소유자를 검사하며, 자기 자신 또는
   interposer 결과이면 fail-closed 처리합니다.

호스트 검사는 이제 과거 `dlsym` interposer 배치를 실제로 재현합니다. 기준
기기에서는 production auto-video/Vulkan-priority 구성으로 codec gateway를
켜 둔 채 수정하지 않은 Sway/wlroots가 제한된 117초 세션 동안
`WLR_RENDERER=vulkan`을 유지했습니다. 컴포지터 `LD_PRELOAD`에는 Firefox
어댑터가 없었고 GLES2 재시도와 종료 고아도 없었습니다. 브라우저 디코드
합격은 별도의 대기 항목입니다.

## 성능 후속 검증

이후 같은 기기에서 낮아진 수치를 추적했지만 compositor 또는 bridge binary
변경은 발견되지 않았습니다. 설치된 wlroots 0.18.2는 Debian 0.18.2-3 원본과
정확히 일치했고, 당시 활성 v11 identity layer, DRM preload, bridge core, vkmark
2025.01 장치 identity, Turnip driver identity도 고점 경로와 같았습니다. 활성
framebuffer 역시 LINEAR 폴백이 아니라 정확한 XB24/QCOM compressed였습니다.

수동으로 만든 LXQt 진단 runner 하나가 정상 desktop client 환경의
`FD_KGSL_ENABLE_DMABUF=1`을 빠뜨린 사실을 확인했습니다. 이를 복구하자 제한된
전체화면 vertex 결과가 9,276에서 13,625 FPS로 상승했습니다. 과거와 같은
`swaymsg exec` 명령을 현재 Sway에서 재현한 결과는 11,824 FPS였고, Waybar를
종료한 결과는 11,480 FPS여서 panel 합성은 회귀 원인이 아니었습니다.

`FD_KGSL_ENABLE_DMABUF=1`은 벤치마크 전용 임시 옵션이 아니라 영구 런타임
계약으로 복구했습니다. `bin/stock-profile-session`과 설치되는 Debian
`/etc/profile.d/99-android-kgsl.sh` payload가 모두 이를 export하며, 소스 및
패키지 검사는 둘 중 하나라도 빠지면 실패합니다.

남은 비교는 전력 상태가 동일하지 않았습니다. 후속 측정 중 KGSL은
`max_freq=607 MHz`, `thermal_pwrlevel=8`로 강제 제한됐고 배터리는 10%,
Android skin thermal status는 light였습니다. 이전 저장 로그는 1.2 GHz,
`thermal_pwrlevel=0`을 노출했고 734 MHz~1.2 GHz 구간을 상당 시간
기록했습니다. 따라서 19,927~20,726 FPS는 실제 측정값으로 유지하되, 렌더러
경로 회귀를 주장하기 전 충전·냉각 상태에서 같은 1.2 GHz 상한으로 재측정해야
합니다. LinDeX는 kernel/OEM/battery/thermal GPU 제한을 강제로 덮어쓰지
않습니다.

### 클럭 상태별 실측 비교

아래 표는 vkmark 전체-suite 점수와 제한된 vertex FPS를 섞지 않습니다. LXQt
7,894점 측정은 재구성한 client 환경에서 `FD_KGSL_ENABLE_DMABUF=1`이 빠졌으므로
607 MHz 비교값에서 의도적으로 제외했습니다.

| 지표와 경로 | 1.2 GHz, `thermal_pwrlevel=0` | 607 MHz, `thermal_pwrlevel=8` | 해석 |
|---|---:|---:|---|
| 원본 Sway/wlroots Vulkan, 1920x1080 Mailbox 전체 vkmark 점수 | 12,660 | 동일 환경 재측정 대기 | 1.2 GHz 기준 점수로만 사용 |
| 과거와 동일한 Sway 명령의 제한된 전체화면 Mailbox vertex FPS | 19,927~20,726 | 11,824 | 제한 상태는 정상 성능의 57.0~59.3%, 정상 클럭은 1.68~1.75배 빠름 |
| dma-buf export 복구 후 제한된 LXQt 진단 vertex FPS | 대응되는 1.2 GHz 측정 없음 | 13,625 | 환경 복구 확인값이며 compositor 간 점수 주장이 아님 |

Android 화면을 켜 Power HAL을 non-interactive에서 interactive로 바꿔도
`max_freq=607 MHz`와 `thermal_pwrlevel=8`은 그대로였습니다. 따라서 실측 성능
하락은 화면 OFF나 wake lock 때문이 아니라 thermal/power 상한 때문이며 Vulkan,
wlroots, UBWC 또는 identity bridge 회귀 증거가 아닙니다.
