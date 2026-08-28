# 제3자 구성요소

[English](THIRD_PARTY.md) | [한국어](THIRD_PARTY.ko.md)

LinDeX는 제3자 소프트웨어의 라이선스를 바꾸지 않습니다. 각 구성요소에는 해당
업스트림 copyright와 라이선스가 적용됩니다.

## 프로젝트 계보

- [av2xn/DOAN](https://github.com/av2xn/DOAN) — MIT License, Copyright
  (c) 2026 av2xn. LinDeX의 초기 Android-Debian 디스플레이 작업과 프로젝트
  방향은 DOAN에서 파생되었습니다.
- [av2xn/Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot)
  — MIT License, Copyright (c) 2026 av2xn. LinDeX의 rootfs installer와
  Debian chroot carrier는 이 프로젝트에서 파생되었습니다.

두 업스트림 MIT 고지는 저장소의 `LICENSE`와 `NOTICE.md`에 보존합니다.

## 런타임 의존성

- [Mesa for Android container](https://github.com/lfdevs/mesa-for-android-container)
  — Mesa 및 패키징별 라이선스. LinDeX는 고정된 릴리스를 다운로드하고
  SHA-256을 검증하며, 압축 파일의 라이선스를 LinDeX로 바꾸지 않습니다.
- [Debian](https://www.debian.org/legal/licenses/) — 패키지별 라이선스.
  Rootfs에는 서로 독립적으로 라이선스된 여러 Debian 패키지가 포함됩니다.
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — MIT License.
  LinDeX v3는 업스트림과 호환되는 wlroots 런타임 구성요소를 사용합니다.
- libdrm, libseat, Wayland, Vulkan, Linux UAPI interface — 각 업스트림 라이선스.
- Android NDK MediaCodec, AHardwareBuffer, EGL, Vulkan interface — Android
  플랫폼 라이선스 조건.

## 데스크톱 프로필

현재 릴리스는 Archcraft Sway Free, LXQt + stock labwc, XFCE + stock labwc의
3개 프로필만 제공합니다. Sway, labwc, LXQt, XFCE와 그 의존성은 각 업스트림
라이선스를 유지합니다. Wayfire, River, Newm은 현재 릴리스 프로필이 아닙니다.

Archcraft Sway Free는 아래 Archcraft 공식 공개 저장소의 checksum 고정 파일을
조정해 사용합니다. 각 저장소는 GPL-3.0을 명시합니다.

- [archcraft-sway](https://github.com/archcraft-os/archcraft-sway), commit
  `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`
- [archcraft-themes](https://github.com/archcraft-os/archcraft-themes), commit
  `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`, `Sweet-Ambar-Blue` 및
  `Qogir-Light` 구성요소
- [archcraft-icons](https://github.com/archcraft-os/archcraft-icons), commit
  `1af3af70ccb233bf26f42162f7e65e4a36803667`, `Ars`, `Qogir`, `Archcraft`
  fallback 구성요소
- [archcraft-cursors](https://github.com/archcraft-os/archcraft-cursors), commit
  `8b7e4633cf8e73502f2cfd396d077edf9304c440`, `Sweet`, `Qogirr-Dark`
  구성요소. 후자의 대응 공개 소스는
  [Qogir cursor tree](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors)입니다.

LinDeX는 Debian 안전 어댑터를 적용합니다. Arch 전용 전원, 밝기, Bluetooth,
welcome/overview, 자동 DPMS 동작을 비활성화하고 필요한 곳에는 Debian 대응 패키지
이름을 사용합니다. 이 조정은 원본 파일의 라이선스를 변경하지 않습니다. 정확한
소스와 archive hash는 `module/profile-assets/SOURCES.lock` 및
`module/profile-assets/APPEARANCE_SOURCES.lock`에 기록됩니다.
`lindex-archcraft-sway-public-assets-v2.tar.gz`는 위의 고정 공개 파일을 LinDeX가
재현 가능하게 묶은 aggregate이며 Archcraft 업스트림 배포 archive가 아닙니다.
Pywal 모드는 설치된 공식 프로필 script
`~/.config/sway/theme/theme.sh --pywal`을 실행하며 추가 시각 자산 라이선스를
도입하지 않습니다. Aggregate에는 Qogirr-Dark 대응 SVG 소스가
`corresponding-source/Qogirr-Dark/src/cursors` 아래에, GPL-3.0 `COPYING` 파일도
함께 실제로 포함됩니다.

Archcraft Sway 제작자의
[Ko-fi 페이지](https://ko-fi.com/s/10f2e87af3)는 감사 표시와 선택적 후원
링크일 뿐입니다. 설치 소스, 필수 결제, 라이선스 조건이 아닙니다. LinDeX는
비공개/유료 Ko-fi archive를 요구하거나 재배포하지 않습니다.
