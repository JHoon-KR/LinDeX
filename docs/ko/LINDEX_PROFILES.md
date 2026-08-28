# LinDeX 프로필

[English](../LINDEX_PROFILES.md) | [한국어](LINDEX_PROFILES.md)

## 릴리스 프로필 목록

WebUI는 정확히 3개 프로필을 제공합니다.

| 프로필 ID | 표시 이름 | 런타임 출처 | 외형 |
|---|---|---|---|
| `sway` | Archcraft Sway Free · 공식 공개 dotfiles | Debian Trixie 및 고정된 Archcraft GitHub 소스 | Debian 안전 어댑터를 적용한 Archcraft 공식 공개 Sway 파일 |
| `lxqt` | LXQt + stock labwc | 서명된 Debian Trixie 패키지 | 업스트림 기본값 |
| `xfce` | XFCE + stock labwc | 서명된 Debian Trixie 패키지 | 업스트림 기본값 |

Wayfire, River, Newm은 릴리스 프로필로 제공하지 않습니다. 과거 후보는 공개 패키지
구성 페이지를 근거로 이들을 표시했지만, LinDeX가 패키징하고 검증할 수 있는
대응 Archcraft 공식 공개 dotfile 세트가 없습니다. 과거 provider 런타임과
단순 fallback 설정은 현재 릴리스 범위에 포함되지 않습니다.

## Archcraft Sway Free 소스 잠금

Sway 프로필은 Ko-fi 다운로드가 아니라 Archcraft 공식 공개 GitHub 저장소를
기준으로 합니다. 릴리스는 다음 GPL-3.0 소스를 고정합니다.

| 구성요소 | 공식 소스 | 고정 commit | 설치 내용 |
|---|---|---|---|
| Sway dotfile 및 배경화면 | [`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway) | `e4d0126d7f236fee50a84fbb0e61498dcf5705e7` | 사용자 Sway 설정 아래의 전체 공개 프로필 archive |
| Dark GTK 테마 | [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes) | `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` | `Sweet-Ambar-Blue` |
| Dark 아이콘 테마 | [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons) | `1af3af70ccb233bf26f42162f7e65e4a36803667` | `Ars` |
| Dark 커서 테마 | [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors) | `8b7e4633cf8e73502f2cfd396d077edf9304c440` | `Sweet` |
| Light GTK 테마 | [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes) | `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` | `Qogir-Light` |
| Light 아이콘 테마 | [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons) | `1af3af70ccb233bf26f42162f7e65e4a36803667` | `Qogir` 및 `Archcraft` fallback |
| Light 커서 테마 | [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors) | `8b7e4633cf8e73502f2cfd396d077edf9304c440` | `Qogirr-Dark`; [대응 Qogir cursor 소스](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors) |

소스 잠금, archive SHA-256, 구성요소 경로는
`module/profile-assets/SOURCES.lock`과
`module/profile-assets/APPEARANCE_SOURCES.lock`에 기록합니다. 외형 파일은
`lindex-archcraft-sway-public-assets-v2.tar.gz`, SHA-256
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`입니다.
이는 위의 고정된 공개 GPL 자산을 LinDeX가 재현 가능하게 묶은 aggregate이며
Archcraft 업스트림 배포 archive가 아닙니다. 패키징과 설치는 인접 digest와 전체
source lock을 검증합니다. Qogirr-Dark 대응 SVG 소스와 라이선스는 실제로
`corresponding-source/Qogirr-Dark/src/cursors` 및
`corresponding-source/Qogirr-Dark/COPYING`에 포함됩니다. 복사된 파일은 계속
GPL-3.0이며 LinDeX MIT 라이선스로 재라이선스하지 않습니다.

이는 과거 Ko-fi 오해를 바로잡습니다. LinDeX는 유료/비공개 Archcraft 프로필
archive를 요구하거나 검색하지 않습니다. 위에 나열한 검토된 공식 공개 GitHub
자료만 포함합니다. 사용자가 Android 저장소에서 다운로드하거나 수동 복사할
필요가 없습니다. Archcraft Sway 제작자의
[Ko-fi 페이지](https://ko-fi.com/s/10f2e87af3)는 선택적 후원 링크일 뿐이며 설치
소스, 필수 결제, 라이선스 조건이 아닙니다.

고정한 dotfile 저장소의 실제 `files/wallpapers/wallpaper.jpg`는 바다와 바위
사진입니다. Archcraft 공개 문서와 갤러리의 합성된 데스크톱 스크린샷에는 어두운
꽃 배경이 보이지만, 그 꽃 원본은 고정한 Sway 저장소에 포함되지 않습니다. 갤러리
스크린샷은 설치 가능한 배경 자산이 아니므로 LinDeX는 합성 스크린샷에서 배경을
잘라 재배포하지 않습니다.

홍보 스크린샷과 현재 공개된 설치 가능 dotfile은 업스트림 revision도 서로
다릅니다. Archcraft가 스크린샷을 commit한 `bc1d170` 시점에는 저장소에 README와
스크린샷만 있었고 설치 가능한 `files/waybar` 트리가 없었습니다. 공개 Sway 설정은
이후 `6702e80`에서 추가됐으며, 현재 고정한 `e4d0126d`는 그 설정의 `Type-2`
Waybar 배치를 선택합니다. 따라서 모든 include가 정상이어도 고정 설정의 bar가
이전 갤러리 bar와 다를 수 있습니다. LinDeX는 스크린샷에만 존재하는 배치를 공식
설치 프로필이라고 표시하지 않습니다.

## Sway 외형 모드

WebUI 빠른 시작은 Sway 전용 선택 3개를 제공하고 다음 세션이 시작되기 전에
선택값을 적용합니다.

- **공식 Dark**는 기본값이며 `Sweet-Ambar-Blue`, `Ars`, `Sweet`를 사용합니다.
- **공식 Light**는 `Qogir-Light`, `Qogir`와 `Archcraft` fallback,
  `Qogirr-Dark`를 사용합니다.
- **Pywal 자동 생성**은 설치된 공식 프로필 script를
  `~/.config/sway/theme/theme.sh --pywal`로 실행합니다.
  `~/Pictures/wallpapers`의 이미지에서 색상을 만들며, 새 프로필에서는 사용자
  파일을 덮어쓰지 않고 번들 공개 Dark/Light 배경화면을 이 경로에 준비합니다.

Pywal은 생성 색상만 바꾸며 checksum 고정 source 자산이나 검증을 대체하지
않습니다. 고정된 `wal` provider 또는 사용 가능한 배경화면이 없으면 다른 외형을
조용히 선택하지 않고 범위가 제한된 프로필/의존성 상태로 실패합니다.

## Debian/chroot 안전 어댑터

LinDeX는 고정된 Archcraft 시각 설정을 보존하고 Android 소유 chroot에서
위험하거나 사용할 수 없는 연동만 변경합니다.

- 전원 메뉴는 Sway 로그아웃만 허용하며 Android suspend, hibernate, reboot,
  poweroff를 실행하지 않습니다.
- Android가 내부 디스플레이와 밝기 정책을 소유하므로 내부 밝기 키 바인딩을
  비활성화합니다.
- Android가 Bluetooth를 소유하므로 Archcraft Bluetooth 바인딩을 비활성화합니다.
- Waybar의 Bluetooth와 backlight 모듈을 생성하지 않습니다. 전자는 chroot에
  BlueZ system bus가 없을 때 Debian Waybar 전체를 종료시킬 수 있고 두 제어 모두
  이 기기에서는 Android가 소유합니다.
- 유지한 공식 Waybar 배터리·네트워크 위젯은 Android의 표준 `battery` 전원
  공급 장치에 명시적으로 연결합니다. 삼성 보조 fuel-gauge의
  0 용량이나 Android 정책 라우팅의 `dummy0`를 잘못 고르는 문제를 막아 거짓
  `0%` 및 `Disconnected` 표시를 방지합니다.
- 네트워크 위젯과 `rofi_network` 도우미를 모두 제거합니다. Sway는 `wlan*`,
  라우팅, 게이트웨이 또는 NetworkManager 상태를 읽지 않으며 Android만
  네트워크 정책을 소유합니다.
- 삼성 표준 배터리 노드도 유효한 `capacity`와 함께 `charge_now=0`을 노출합니다.
  Waybar 0.12가 charge counter를 우선하므로 LinDeX는 읽기 전용 custom module로
  표준 capacity를 표시합니다.
- 연결되지 않는 PulseAudio 위젯은 Android `STREAM_MUSIC` 위젯으로 교체합니다.
  현재 Android 미디어 볼륨 비율을 표시하고, 스크롤하면 Android 볼륨을 한 단계씩
  올리거나 내리며, 클릭하면 0과 마지막 0이 아닌 값 사이를 전환합니다. Android
  호스트 감시자가 2초 제한을 둔 `cmd media_session`을 실행하고, 기존 tmpfs 기반
  `/run/android-drm`에서 비공개·크기 제한 상태/명령 파일만 교환합니다. chroot에
  `cmd`, `/system`, `/apex`를 마운트하지 않고 ALSA/PulseAudio 출력도 열지 않습니다.
  MPD는 자동 시작하거나 Waybar에 생성하지 않으며 release 빌드는 영구 볼륨 로그를
  만들지 않습니다.
- 이 root chroot에는 PolicyKit 에이전트가 등록할 logind 사용자 세션이 없으므로
  `lxpolkit`, `xfce-polkit`, `lxqt-policykit`을 설치하지 않습니다.
- `wofi`, `kanshi`, `wlogout`은 Archcraft의 공개 타 배포판 의존성 목록과 맞추기
  위해 설치합니다. 고정한 무료 프로필은 계속 Rofi launcher를 사용하며, leased
  외부 출력 정책과 Android 전원 제어는 Android/WebUI가 담당합니다.
- `hyprpicker`와 `hyprlock`은 Debian backports의 실제 패키지여야 합니다. 업그레이드
  때는 과거 LinDeX 호환 스크립트의 정확한 해시만 제거하여 가짜 준비 판정이나 실제
  바이너리 가림이 발생하지 않게 합니다.
- Leased 외부 출력의 자동 잠금과 DPMS를 비활성화합니다.
- Debian에서 사용할 수 없는 Arch 전용 Sway Overview와 welcome/service 경로를
  비활성화합니다.
- 배포판 이름이 다른 항목만 대응합니다. `mako`는 `mako-notifier`,
  `xorg-xwayland`는 `xwayland`, `python-pywal`은 고정된 pywal16 provider에
  대응합니다.

어댑터는 배경화면, Waybar 배치, launcher 테마, 색상, GTK 테마, 아이콘, 커서를
단순 fallback 데스크톱으로 교체하지 않습니다. 설치 후 source marker에 고정
commit, archive digest, GPL-3.0 라이선스, 어댑터 revision을 기록합니다.

## 공통 기본 의존성

3개 프로필은 공통 Debian base를 공유하고 chroot PolicyKit 에이전트는
제외합니다. 설치 관리자는 공개 프로필 앱과 내부 전이 런타임 라이브러리를
분리하므로 의존성 패키지를 추가 데스크톱 앱처럼 표시하지 않습니다. LXQt는
`lxqt-core` 대신 명시적 구성요소 목록을 사용해 `pcmanfm-qt`, 패널, 세션,
runner, 테마, Qt Wayland 연동은 유지하면서 `lxqt-policykit`을 끌어오지 않습니다.
패키지, 명령, 자산, checksum 요구 사항을 모두 통과해야 준비 상태가 되며 오래된
ready marker만으로는 충분하지 않습니다.

LXQt와 XFCE는 stock Debian labwc를 사용하며 Archcraft Sway dotfile이나 외형
자산을 상속하지 않습니다.

## 바탕화면 아이콘과 파일 관리자

- Archcraft Sway에는 의도적으로 바탕화면 아이콘 관리자가 없습니다. 상단 왼쪽의
  작은 아이콘은 바탕화면 바로가기가 아니라 Sway workspace 버튼입니다. 참고
  화면의 휴지통은 대개 열린 Thunar 창 안에 표시된 항목입니다.
  `xfce4-terminal`이나 `xfce4-settings`를 설치해도 Sway 바탕화면 아이콘은
  생기지 않습니다. 공식 프로필에서는 `Super+Shift+F`로 Thunar를 엽니다.
- LXQt는 LXQt 세션 모듈로 시작되는 `pcmanfm-qt`가 Wayland 바탕화면을
  담당합니다. 기본 바탕화면 바로가기는 홈, 휴지통, 컴퓨터, 네트워크이며 파일 및
  휴지통 연동을 위해 `gvfs`도 포함합니다.
- XFCE의 Debian `xfce4` 메타패키지에는 이미 `xfdesktop4`,
  `xfce4-settings`, Thunar가 포함되고 LinDeX는 `xfce4-terminal`도 명시합니다.
  XFCE 바탕화면 아이콘은 터미널이나 설정 앱이 아니라 `xfdesktop4`가 그립니다.

## 원버튼 설정

선택한 프로필에서 **지금 시작**은 다음 범위가 제한된 작업을 수행합니다.

1. 공통 base와 선택한 3개 프로필 계약을 검증합니다.
2. Allowlist에 있는 서명된 Debian 패키지를 설치합니다.
3. Sway에서는 고정된 Archcraft 공식 공개 자산을 검증·설치하고 선택한 Dark,
   Light 또는 Pywal 모드를 적용합니다.
4. 선택한 KGSL Mesa payload를 설치하거나 검증합니다.
5. 설치 패키지, 진입 명령, 자산 marker, 브리지를 검증합니다.
6. 물리 DP와 유효한 EDID가 있을 때 컴포지터를 시작합니다.

자산 또는 checksum 검사가 실패하면 `profile-unavailable`로 보고하며, 비공식
archive를 네트워크에서 검색하거나 minimal 프로필로 조용히 폴백하지 않습니다.
기존의 비 Archcraft 사용자 설정은 조용히 덮어쓰지 않고 보존합니다.

## 검증 상태

현재 후보에서 호스트 측 패키지/자산 및 chroot PolicyKit 에이전트 제외 계약이
통과합니다. Archcraft Sway Free, LXQt/labwc, XFCE/labwc의 정확한 패키지 모듈
시작, 중지, 강제 분리, 재연결 매트릭스는 릴리스 게이트로 남아 있습니다.
[검증 상태](VALIDATION_STATUS.md)를 확인하세요.
