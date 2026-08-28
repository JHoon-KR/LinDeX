# 설치

[English](../INSTALLATION.md) | [한국어](INSTALLATION.md)

> LinDeX v3는 공개 전 소프트웨어입니다. 현재 기준 대상은 Adreno 830,
> KernelSU, Debian 13 ARM64를 사용하는 Samsung SM-S937N입니다. 다른 기기는
> 자체 검증 매트릭스를 통과하기 전까지 지원된다고 판단하지 마세요.

## 사전 조건

- KernelSU와 모듈 WebUI를 사용하는 ARM64 Android
- `/dev/kgsl-3d0`으로 노출된 Qualcomm Adreno
- `/dev/dri/card0`으로 노출된 MSM/SDE KMS
- USB-C DisplayPort Alt Mode 및 유효한 EDID를 제공하는 모니터
- rootfs, 패키지, Mesa, 사용자 데이터를 위한 충분한 여유 공간
- 보존해야 하는 기존 `/data/local/debian` 데이터의 백업

모듈 레이아웃은 일반적인 root 모듈 패키징과의 호환성을 유지하도록 구성하지만,
현재 공개 설치 절차와 검수 게이트는 KernelSU를 사용합니다. 다른 root 관리자는
별도로 테스트할 때까지 미검증으로 문서화해야 합니다.

## 새 설치 또는 업데이트

1. 기존 Debian chroot 안의 데이터를 백업합니다.
2. LinDeX가 실행 중이면 WebUI에서 중지합니다.
3. DP 케이블 또는 USB-C 비디오 어댑터를 물리적으로 분리합니다.
4. KernelSU 모듈 설치 관리자에서 rootfs 포함 LinDeX ZIP을 설치합니다.
5. 설치 결과를 읽고 필수 기기 또는 압축 파일 검사가 실패하면 중단합니다.
6. Android를 일반 재부팅해 root 모듈을 활성화합니다.
7. LinDeX WebUI를 열고 표시된 빌드 flavor와 rootfs 상태가 예상과 같은지
   확인합니다.
8. 프로필 하나를 선택합니다. Archcraft Sway Free에서는 **공식 Dark**,
   **공식 Light**, **Pywal 자동 생성** 중 하나도 선택한 뒤 **지금 시작**을
   누릅니다. 첫 시작에는 Debian 패키지, 고정된 KGSL Mesa payload, Sway 선택 시
   검증된 Archcraft Sway 자산을 설치할 수 있습니다.
9. 안내가 나올 때 또는 자동 시작 설정을 마친 뒤에만 DP를 연결합니다.

새 설치는 `debianfs-arm64.tar.xz`를 검증해 `/data/local/debian`에 풉니다.
DRM lease 브리지, 프로필 관리자, Mesa 설정, 세션 컨트롤러, 입력 middleware,
릴리스 코덱 서비스와 검증된 ARM64 ADVC VA-API driver를
`/opt/android-drm-lease-kit/codec/vaapi` 아래에 설치합니다. `/system`
오버레이는 만들지 않습니다.

Video acceleration이 **Auto**이면 packaged driver, production broker socket,
`/dev/dri/renderD128`, `/proc/self/fd`, `/sys/class/drm/renderD128`이 모두
존재할 때만 LinDeX가 libva와 GStreamer 앱에 ADVC를 광고합니다. 하나라도
실패하면 관련 환경 변수는 unset으로 남고 desktop은 ADVC 광고 없이 시작합니다.
**Disabled**는 항상 unset 상태를 유지합니다.

## 첫 프로필 설정

WebUI는 정확히 Archcraft Sway Free, LXQt + stock labwc, XFCE + stock labwc를
제공합니다. 모든 프로필은 codec·graphics runtime을 포함한 공통 기본 의존성을
받지만 LinDeX는 root chroot에 PolicyKit 에이전트를 추가하지 않습니다. 같은
기본 설치에서 FFmpeg, `vainfo`, GStreamer VA-API plugin과 도구, 그리고
`h264parse` 같은 codec parser를 제공하는 Bad plugin 묶음도 설치하므로 정상 ADVC
driver가 parser 누락 때문에 숨겨지지 않습니다. 패키지는 서명된 Debian 저장소에서
설치합니다. Sway 프로필은 [프로필](LINDEX_PROFILES.md)에
기록한 고정 commit의 Archcraft 공식 공개 GPL-3.0 dotfile·배경화면과 Dark/Light
GTK 테마·아이콘·커서를 추가로 검증해 설치합니다. LinDeX가 외형 세트를 재현
가능한 aggregate로 패키징하며, 이는 Archcraft 업스트림 배포 archive 또는 Ko-fi
archive가 아닙니다. Pywal 선택은 `~/.config/sway/theme/theme.sh --pywal`을
사용합니다. Wayfire, River, Newm은 릴리스 선택 항목이 아닙니다. 패키지, 실행
명령, 자산, checksum 검증을 모두 통과해야 설치 완료로 판정합니다.

첫 프로필이 패키지 또는 provider 설치 단계를 표시하는 동안 WebUI를 닫거나
DP를 연결하지 마세요. 설정이 실패하면 짧은 WebUI 오류를 기록하고, 진단이
필요할 때만 dev 빌드를 사용하세요.

## Mesa

기본 설정은
[`lfdevs/mesa-for-android-container`](https://github.com/lfdevs/mesa-for-android-container)의
고정된 KGSL 지원 Mesa 압축 파일을 다운로드하고 SHA-256을 검증합니다. 다운로드
또는 digest 검사가 실패하면 활성 Mesa 파일을 교체하지 않고 중단합니다. 이
고정 fork는 Android Gallium loader 이름 `kgsl`을 사용합니다. KGSL 렌더링에서
이를 업스트림 DRM `msm` loader로 대체하지 마세요.

권장 **KGSL + 패치 Turnip** 모드는 이 기본 압축 파일 하나만 설치합니다. 여기에는
KGSL OpenGL/GLES 경로와 패치된 Turnip Vulkan 드라이버가 모두 들어 있습니다.
**KGSL + 비패치 Turnip**은 기본 Turnip이 실패하는 기기를 위한 호환 옵션으로,
기본 압축 파일을 먼저 설치한 다음 별도 비패치 Turnip을 덮어씁니다. 작동하는
하드웨어 Vulkan 렌더러를 찾지 못하면 기존처럼 하드웨어 KGSL GLES로 폴백합니다.

## 설치 검증

설치가 정상이라고 판단하기 전에 다음을 모두 확인합니다.

- WebUI가 예상한 `release` 또는 `dev` flavor를 표시한다.
- 선택한 프로필이 준비 상태이며 브리지 payload가 검증된다.
- 유효한 EDID가 나타난 뒤에만 DP 상태가 분리에서 연결로 바뀐다.
- 시작, 중지, 강제 케이블 분리, 재연결이 깨끗한 새 세션을 만든다.
- Android `/system`, `/vendor`, `/product`가 그대로 유지된다.
- release 모드가 영구 설정, 세션, 코덱 로그를 만들지 않는다.
- 선택한 USB 입력 모드가 세션 중지 시 기기를 해제한다.

3개 프로필 전체 매트릭스와 아직 비활성인 디코드 DRM PRIME 경로는 릴리스
게이트로 남아 있습니다. 제한된 패키지 모듈 앱 인코드는 기준 기기에서 이미
통과했습니다. [검증 상태](VALIDATION_STATUS.md)를 확인하세요.

## 제거

모듈 제거는 활성 세션을 중지하고 lease를 닫고 중첩 chroot mount를 해제한 뒤
`/data/local/debian`과 `/data/adb/debian-drm-lease-kit`을 모두 제거합니다.
이는 chroot 안의 사용자 데이터를 의도적으로 삭제합니다. 먼저 백업하세요.

## 소스 빌드

Git 저장소에는 Debian rootfs나 다운로드한 Mesa 압축 파일이 없습니다. 패키저는
명시적으로 제공되고 검토된 rootfs carrier를 입력으로 받아 release와 dev ZIP을
별도로 만듭니다.

```powershell
./scripts/package-v3-module.ps1 `
  -BaseModuleZip C:\path\to\debian_chroot.zip `
  -Flavor release

./scripts/verify-v3-module.ps1 `
  -ModuleZip ./dist/LinDeX-v3.0.4.zip `
  -ExpectedFlavor release
```

공개에는 압축 파일 빌드 성공뿐 아니라 전체 [릴리스 체크리스트](RELEASING.md)가
필요합니다.
