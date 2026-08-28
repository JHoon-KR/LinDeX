# LinDeX

[English](README.md) | [한국어](README.ko.md)

> **공개 전 상태:** LinDeX v3의 첫 공개 릴리스를 준비하고 있습니다. 호스트
> 검사와 패키지 모듈의 앱 인코드 검증은 통과했습니다. 3개 프로필 전체 기기
> 매트릭스와 디코드 DRM PRIME 게이트는 아직 완료되지 않았습니다.
> [검증 상태](docs/ko/VALIDATION_STATUS.md)를 확인하세요.

LinDeX는 Android가 휴대폰 화면의 소유권을 유지하는 동안 물리 USB-C
DisplayPort 출력에서 ARM64 Debian Wayland 데스크톱을 실행합니다. MSM/SDE
디스플레이 하드웨어를 사용하는 Qualcomm Adreno/KGSL 기기를 대상으로 하며,
**JHoon**이 관리합니다.

LinDeX는 **av2xn**의 [DOAN](https://github.com/av2xn/DOAN)과
[Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot)에서
출발했습니다. 두 프로젝트의 MIT 고지는 [LICENSE](LICENSE)와
[NOTICE.ko.md](NOTICE.ko.md)에 보존되어 있습니다. LinDeX는 독립적인 후속
프로젝트이며, 해당 원본 프로젝트·기기 제조사·Android·데스크톱 프로젝트와의
제휴를 주장하지 않습니다.

## v3가 제공하는 기능

- 수정하지 않은 Wayland 컴포지터를 위한 작은 DRM lease 브리지. LinDeX는
  컴포지터 패치나 컴포지터별 디스플레이 포크를 요구하지 않습니다.
- Archcraft Sway Free, LXQt + stock labwc, XFCE + stock labwc의 3개 선택 프로필.
- Archcraft 공식 공개 Sway dotfile·배경화면과 공개 GPL-3.0 Dark/Light GTK
  테마·아이콘·커서 자산의 고정 버전 및 checksum 검증 사본. LinDeX가 문서화한
  공개 소스에서 외형 파일을 재현 가능하게 묶으며, 이 aggregate는 Archcraft
  업스트림 배포 archive가 아닙니다. Ko-fi나 사용자 제공 archive가 필요하지 않습니다.
- 프로필 설정, DP 시작/중지, 디스플레이 모드, 직접 스캔아웃 호환 설정,
  USB 입력 라우팅, 비디오 가속을 제어하는 KernelSU 모듈 WebUI.
- 중지·실패·물리 케이블 분리 시 정확한 세션/프로세스 그룹 소유권 및 lease 정리.
- 별도의 `release`와 `dev` 패키지. release 모드는 설정/세션/코덱 영구 로그를
  남기지 않고, dev 모드는 크기가 제한된 진단 로그를 보존합니다.
- Android `/system`, `/vendor`, `/product` 아래에 파일이나 오버레이를
  설치하지 않습니다.

## 네이티브 GPU 가속

LinDeX에서 “네이티브”란 Debian 앱과 Wayland 컴포지터가 Mesa를 통해 휴대폰의
실제 Adreno GPU에 작업을 제출한다는 뜻입니다. 소프트웨어 래스터라이징이나
Android 데스크톱 스트리밍, CPU로 Android framebuffer를 복사하는 방식이
아닙니다. 그렇다고 모든 창이 언제나 합성 없이 디스플레이 plane에 바로
표시된다는 뜻도 아닙니다.

```text
Debian Wayland 앱
  |-- Vulkan -> Mesa Turnip --------------------|
  `-- OpenGL/GLES -> Mesa Freedreno `kgsl` -----|-> /dev/kgsl-3d0
                                                  -> dma-buf + native fence
                                                  -> stock Sway/labwc

Android 디스플레이 소유자 -> 제한된 DRM lease -> /dev/dri/card0 (MSM/SDE KMS)
                                                   -> 외부 DP plane/connector
```

두 경로는 서로 교체 가능한 GPU 드라이버가 아니라 협력하는 별도 커널
인터페이스입니다.

- `/dev/kgsl-3d0`은 Adreno를 위한 Qualcomm Android stock kernel의 명령 제출
  경로입니다. Turnip은 Vulkan에, 고정된 Mesa fork의 Freedreno/Gallium `kgsl`
  경로는 OpenGL/GLES에 이를 사용합니다.
- `/dev/dri/card0`과 `/dev/dri/renderD128`은 MSM/SDE DRM 디스플레이 쪽입니다.
  LinDeX는 `card0`에서 외부 connector/CRTC/plane을 lease하고 DRM identity를
  allocation 및 디스플레이 매칭에 사용합니다. 이 노드를 KGSL을 대체하는 Adreno
  렌더러라고 가장하지 않습니다.
- Root 권한은 chroot 생성, 제한된 기기 접근, DRM lease 중개에 필요합니다.
  Root라고 해서 별도의 더 빠른 범용 GPU API가 나타나는 것은 아닙니다. 이 Android
  kernel에서 `kgsl`을 upstream Mesa의 일반 DRM `msm` loader로 교체하려면 해당
  kernel이 일반 Adreno DRM render node와 ABI를 실제로 제공해야 합니다.

릴리스 컴포지터는 수정하지 않은 Debian 패키지입니다. Archcraft Sway는 Sway
1.10.1/wlroots 0.18.2를, LXQt와 XFCE는 stock labwc를 사용합니다. 여기서
“무수정”은 어댑터도 없다는 뜻이 아닙니다. 프로세스 범위 DRM lease 브리지가
소유한 컴포지터에만 이미 권한이 부여된 KMS descriptor를 전달하고, fail-closed
Vulkan explicit layer가 정확히 일치하는 Qualcomm/Turnip/기기 tuple에만 누락된
DRM identity metadata를 제공합니다. 이 디스플레이 경로는 wlroots, Sway, labwc,
Mesa, kernel 또는 Android 읽기 전용 파티션을 패치하지 않습니다.

### 렌더러 선택과 폴백

세션 시작 시 LinDeX는 `vulkaninfo`로 설치된 ICD를 검사하고 lavapipe 같은
software renderer를 거부합니다. 정확한 기기 identity gate를 통과하면 hardware
Turnip Vulkan renderer를 선택합니다. `FD_KGSL_ENABLE_DMABUF=1`은 Mesa의 KGSL
dma-buf export 경로를 활성 상태로 유지합니다. 자동 모드에서 hardware Vulkan을
사용할 수 없으면 hardware Freedreno/GLES2 경로로 시작하며, software rendering은
성공한 폴백으로 인정하지 않습니다. Vulkan 강제 선택은 렌더러를 조용히 바꾸지
않고 fail-closed 처리합니다.

### dma-buf, UBWC, 합성과 직접 스캔아웃

렌더링된 이미지는 명시적인 format, modifier, fence metadata를 가진 dma-buf로
KGSL에서 MSM/SDE 쪽으로 전달됩니다. `XB24`/`XR24`는 데스크톱 픽셀 형식이고
QCOM/UBWC와 LINEAR는 메모리 배치 방식입니다. 코덱 문서의 NV12 영상 형식과는
별개입니다.

새 설치의 기본값은 exact-gated **UBWC 우선** 정책입니다. LINEAR 폴백을 유지하며
연결된 plane과 live device identity가 검증된 QCOM 후보와 일치하지 않으면 원래
정보를 바꾸지 않습니다. 불투명 전체화면 client의 buffer와 leased primary
plane이 합의하면 XB24/QCOM으로 직접 스캔아웃할 수 있습니다. Waybar, 알림,
popup, 변환된 surface, 색 처리, software cursor가 보이면 scene이 부적격해질 수
있고 stock wlroots는 정상적으로 XR24 LINEAR 합성으로 돌아갑니다. LinDeX는
컴포지터의 overlap, transform, cursor, color, synchronization 검사를 우회해서
직접 스캔아웃을 강제하지 않습니다.

따라서 WebUI의 세 정책은 의도적으로 의미가 다릅니다.

| 정책 | 동작 |
|---|---|
| **UBWC 우선** | 적격 전체화면 스캔아웃에서 정확히 검증된 XB24/QCOM 후보를 우선하며 LINEAR 폴백 유지 |
| **자동** | stock wlroots의 형식 우선순위를 바꾸지 않고 검증 후보만 추가. 호환성 우선이며 일반적으로 합성 LINEAR |
| **LINEAR** | 호환성을 위해 modifier 사용 비활성화 |

### 기준 기기 증거와 주장 범위

다음 수치는 Galaxy S25 Edge/Adreno 830 한 대에서 얻은 증거이며 다른 기기의
성능을 보장하지 않습니다.

| 검사 | 측정 결과 |
|---|---|
| 수정하지 않은 Sway/wlroots 렌더러 | KGSL 기반 hardware `WLR_RENDERER=vulkan`/Turnip, llvmpipe/lavapipe 없음 |
| 전체화면 1920x1080 Mailbox vkmark | Vulkan 12,660점, 직전 GLES2 경로 4,078점 |
| 제한된 전체화면 vertex workload | GPU 상한 1.2GHz에서 19,927-20,726 FPS, thermal 상한 607MHz에서 11,824 FPS |
| 현재 v3.0.3-dev 전체 suite snapshot | 11,013점, 표본 GPU load 평균 88.8%·최대 99%, 120개 표본 중 89개에서 thermal 제한 존재 |
| 동일 lease 직접 스캔아웃 | strict 우선 정책에서 활성 framebuffer 91개 중 88개가 XB24/QCOM. LINEAR 3개는 진입/종료 전환 |
| 합성 대조군 | Waybar가 표시된 상태의 활성 framebuffer는 예상대로 XR24 LINEAR |

vkmark 점수와 개별 scene FPS는 서로 다른 지표입니다. Present mode, scene, panel
표시 여부, 해상도, GPU clock 상한, 배터리 상태, OEM thermal 정책이 같아야 두
결과를 비교할 수 있습니다. LinDeX는 GPU clock을 강제하거나 Android thermal
정책을 우회하지 않습니다.

디스플레이 모드는 연결된 sink의 EDID 모드를 기본으로 따릅니다. 수동 100, 120,
144Hz 항목은 자동 복구가 없는 실험적 선택이며 실제 적용 주사율은 휴대폰,
어댑터, 케이블, 모니터 조합에 따라 달라집니다. 현재 기준 검수 세션은
1920x1080 60Hz를 사용했습니다.

기준 기기의 Vulkan, UBWC 우선, 일반 전체화면 직접 스캔아웃 경로는 구현했고
범위가 제한된 검증도 통과했습니다. 이것은 범용 또는 “100%” 가속 주장과는
다릅니다. 정확한 태그 패키지 lifecycle 검증, 강제 물리 DP 분리 뒤 안정적인
복구, Android 내부 화면 경쟁 검사, EGL-only 기기 폴백, 추가 Qualcomm 기기
검증은 여전히 릴리스/이식성 게이트입니다. 전체화면 스캔아웃을 위해 Waybar를
숨기고 복원하는 선택형 deterministic Sway IPC state watcher도 아직 검수되지
않았으며, bar를 계속 표시하면 의도대로 합성 경로를 유지합니다.

두 package flavor의 실제 runtime은 같습니다. `release`는 WebUI 상태는 유지하되
영구 setup/session/codec 진단 로그를 만들지 않고, `dev`는 오류 재현을 위한
크기가 제한된 순환 진단만 보존합니다. [아키텍처](docs/ko/ARCHITECTURE.md),
[Vulkan DRM identity](docs/ko/VULKAN_DRM_IDENTITY.md),
[검증 상태](docs/ko/VALIDATION_STATUS.md)를 확인하세요.

## 설치

1. 기존 LinDeX Debian chroot 안의 필요한 파일을 백업합니다.
2. DP 케이블 또는 USB-C 디스플레이 어댑터를 물리적으로 분리합니다.
3. KernelSU 모듈 설치 관리자로 rootfs 포함 LinDeX ZIP을 설치합니다.
4. Android를 일반 재부팅해 root 모듈을 활성화합니다.
5. LinDeX WebUI를 열고 프로필을 고릅니다. Archcraft Sway Free에서는
   **공식 Dark**, **공식 Light**, **Pywal 자동 생성** 중 하나를 선택합니다.
   기본값은 Dark이며 Pywal은 번들 또는 사용자 배경화면에서 색상을 만듭니다.
6. **지금 시작**을 누릅니다.
7. 안내가 나오면 DP를 연결하거나, 설정 성공 후 DP 자동 시작을 켭니다.

새 설치에서는 Debian 13(Trixie)을 `/data/local/debian`에 풉니다. 모듈을
제거하면 이 chroot와 내부 데이터도 제거됩니다. 설치하거나 제거하기 전에
[전체 설치 안내](docs/ko/INSTALLATION.md)를 읽으세요.

## 프로필과 출처

Archcraft Sway Free는 공식 공개
[`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway)
저장소의 commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`(GPL-3.0)을
기준으로 합니다. Dark/Light 공개 테마·아이콘·커서 소스의 개별 고정 버전과
출처는 [프로필](docs/ko/LINDEX_PROFILES.md)에 기록합니다. LinDeX는 작은
Debian/chroot 안전 어댑터만 적용합니다. Android가 소유하는 전원 동작, 내부
밝기와 Bluetooth 키 바인딩, 자동 잠금/DPMS는 비활성화하고 시각 자산은 고정된
공개 파일을 유지합니다. LXQt와 XFCE는 서명된 Debian 패키지와 stock labwc를
사용합니다.

LinDeX는 PolicyKit 에이전트를 추가하지 않습니다. root chroot에는 사용할 수 있는
logind 사용자 세션이 없으므로 `lxpolkit`, `xfce-polkit`, `lxqt-policykit`은
오해를 부르는 `No session for PID` 오류만 표시합니다. LXQt 프로필은
`lxqt-core` 메타패키지 대신 구성요소를 명시적으로 설치해 PolicyKit 에이전트는
끌어오지 않으면서 `pcmanfm-qt` 데스크톱/파일 관리는 유지합니다. Wayfire,
River, Newm은 대응하는 Archcraft 공식 공개 dotfile 세트를 패키징하지 않으므로
릴리스 프로필이 아닙니다. [프로필](docs/ko/LINDEX_PROFILES.md)과
[제3자 고지](THIRD_PARTY.ko.md)를 확인하세요.

Archcraft Sway 제작자에게 감사드립니다.
[Ko-fi 페이지](https://ko-fi.com/s/10f2e87af3)는 선택적 후원 링크일 뿐이며,
설치 소스·필수 결제·라이선스 조건이 아닙니다.

## 현재 검증 요약

| 영역 | 현재 공개 가능한 주장 | 릴리스 게이트 |
|---|---|---|
| 호스트 빌드 및 단위/통합 테스트 | 현재 소스 후보에서 통과 | 태그 커밋에서 재실행 필요 |
| DRM lease 및 케이블 분리 정리 | 기준 기기 집중 검증과 fixture 검증 완료 | 최종 패키지 모듈 재연결 매트릭스 대기 |
| 3개 프로필 | 패키지/자산 계약과 공통 의존성 검증 완료 | 최종 시작/중지/hotplug 기기 매트릭스 대기 |
| 인코드 | H.264 CB와 HEVC Main이 QTI 하드웨어에서 각각 720p60 120/120프레임을 84.9·81.1fps로 완주. PRIME/OBS 입력은 CPU raw-pixel 복사 0회와 GPU 변환 1회, 일반 FFmpeg upload는 CPU 복사 1회 | Vulkan producer 최적화는 호스트/NDK 검증 후 최종 패키지에서 측정 반복 |
| 디코드 byte 경로 | 기준 기기 PASS: 720x360 NV12, stride 768, slice height 384, 436,176 byte, raw-frame CPU 복사 1회 | 정확한 release 산출물에서 반복 필요 |
| 디코드 QCOM UBWC PRIME | H.264 Main+B 180/180, HEVC Main+B 120/120, VP9 Profile 0 120/120이 PRIME, release, EOS, codec stop을 통과. H.264에는 정확한 bounded reorder gate 적용 | HEVC Main과 VP9 Profile 0은 각 live preflight 통과 뒤 공개 베타로 광고. 화면 재생·seek는 앱별 시험이며 H.264 High, HEVC Main10, AV1은 계속 숨김 |
| 디코드 modifier `0` LINEAR | 동일 장기 실행이 content 일치, Vulkan repack 1회, CPU raw-pixel 복사 0회로 통과. 평균 repack 시간은 1.38~1.74ms | 각 패키지 앱에서 화면 재생, seek, EOS, 종료를 반복 검증 |
| Firefox RDD 샌드박스 게이트웨이 | 설치된 v3.0.3-dev에서 seek 30회, document reload 10회 통과: seccomp mode 2, RDD KGSL FD 0, `c2.qti.avc.decoder`, decoded 766/dropped 5, hardware output 1,757회, LINEAR export 1,758회, timeout/비동기 실패 0회, session 종료·재시작 고아 0 | 공개 tagged release ZIP에서 반복하고 gateway의 정확한 프레임별 fence 계수를 완료해야 함 |
| 직접 스캔아웃 | 원본 Debian wlroots 0.18.2/Sway 1.10.1에서 PASS: UBWC 우선 정책은 활성 FB 91회 중 88회가 정확한 XB24/QCOM이었고 LINEAR 3회는 진입·종료 전환 프레임. Waybar 복원 후 데스크톱은 XR24 LINEAR로 복귀 | 호환성 우선 `auto`는 후보만 추가하므로 90/90 XR24 LINEAR. 합성 데스크톱 UBWC에는 버전별 wlroots 어댑터 또는 패치가 아직 필요 |

“CPU 복사 0회”는 GPU 작업도 0회라는 뜻이 아닙니다. 검증된 인코드 후보는
제한된 GPU blit 1회를 사용합니다. 디코드 capability는 각각 fail-closed입니다.
검증된 H.264 QCOM 경로는 정확한 산출물 게이트에서만 노출합니다. HEVC Main과
VP9 Profile 0은 각 live QTI/PRIME/120-of-120 preflight가 통과한 경우에만 공개
베타로 광고합니다. 앱 화면 출력과 seek는 호출자가 위조할 수 있는 승인 조건이
아니라 별도로 축적할 호환성 증거입니다. Modifier 값 `0`은 DRM LINEAR
메타데이터라고 명시된 경우에만 유효하며 누락된 메타데이터로 취급하지 않습니다.
자세한 내용은
[코덱 및 zero-copy 상태](docs/ko/VIDEO_ZERO_COPY_STATUS.md)를 확인하세요.
앱 관점의 `auto|linear|qcom` 규칙과 기존 selector 이전 방법은
[VA-API modifier 정책](docs/ko/VAAPI_MODIFIER_POLICY.md)을 확인하세요.

## 안전 경계

- 설치하거나 업데이트하기 전에 DP를 분리합니다.
- 모듈 설치 또는 업데이트 후 Android를 일반 재부팅합니다.
- LinDeX를 위해 Android 읽기 전용 파티션을 다시 마운트하거나 수정하지 않습니다.
- 엄격한 zero-copy 모드는 CPU raw-pixel 복사로 조용히 폴백하지 않습니다.
- 수동 100/120/144Hz 모드는 실험적이며 자동 롤백이 없습니다.
- 제거 전에 chroot를 백업합니다.

[안전 및 복구](docs/ko/SAFETY.md)와
[문제 해결](docs/ko/TROUBLESHOOTING.md)을 확인하세요.

## 문서

[문서 인덱스](docs/ko/README.md)에서 사용자·아키텍처·프로필·코덱·릴리스·기여
안내의 영문/한국어 대응본을 찾을 수 있습니다. 소스 프로토콜 참고자료와 릴리스
엔지니어링 문서는 사용자 문서와 분리해 표시합니다.

## 소스와 릴리스

저장소에는 소스, 모듈 파일, 검증 스크립트, 공개 문서와 Archcraft Sway 프로필에
필요한 checksum 고정 공식 공개 자산이 포함됩니다. Rootfs 압축 파일, 다운로드한
Mesa, 폐기된 provider 압축 파일, 빌드 산출물, 기기 로그, 인증정보, Ko-fi/비공개
자산, 과거 컴포지터 실험은 의도적으로 제외합니다. 공개 모듈 ZIP은 검토된
입력으로 만든 rootfs 포함 릴리스 산출물입니다.

[릴리스 패키징](docs/ko/RELEASING.md), [기여](CONTRIBUTING.ko.md),
[보안](SECURITY.ko.md)을 확인하세요.

## 라이선스

LinDeX 코드는 MIT License로 배포됩니다. 포함되거나 다운로드되는 제3자
구성요소는 각 업스트림 라이선스를 유지합니다. [LICENSE](LICENSE),
[NOTICE.ko.md](NOTICE.ko.md), [THIRD_PARTY.ko.md](THIRD_PARTY.ko.md),
[라이선스 및 출처](docs/ko/LICENSE_AND_ATTRIBUTION.md)를 확인하세요.
