# 라이선스 및 출처

[English](../LICENSE_AND_ATTRIBUTION.md) | [한국어](LICENSE_AND_ATTRIBUTION.md)

## LinDeX 라이선스

LinDeX 코드는 [MIT License](../../LICENSE)로 배포됩니다. `LICENSE` 파일이
법적으로 기준이 되는 원문입니다. 이 안내는 저장소 출처를 요약하며 해당
라이선스를 대체하지 않습니다.

## 프로젝트 계보

LinDeX는 **av2xn**의 다음 MIT 라이선스 프로젝트에서 일부 파생되었습니다.

- [DOAN](https://github.com/av2xn/DOAN)
- [Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot)

원본 `Copyright (c) 2026 av2xn` 고지는 `LICENSE`와 `NOTICE.md`에 보존합니다.
LinDeX 변경은 JHoon과 LinDeX 기여자에게 귀속됩니다.

## 제3자 소프트웨어

LinDeX는 제3자 구성요소의 라이선스를 바꾸지 않습니다. Debian 패키지, Mesa,
wlroots, Wayland, libdrm, libseat, Android interface, 컴포지터, provider 소스는
각 업스트림 라이선스를 유지합니다. 해당 조건이 요구하면 바이너리 릴리스
담당자는 대응하는 라이선스/소스 자료를 포함하거나 제공해야 합니다.

구성요소 목록과 업스트림 링크는 [THIRD_PARTY.ko.md](../../THIRD_PARTY.ko.md)를
확인하세요. Checksum은 파일 identity를 증명하지만 새 라이선스를 부여하지 않습니다.

## Archcraft 경계

Archcraft Sway Free 릴리스 프로필에는 다음 Archcraft 공식 공개 GitHub 소스의
GPL-3.0 파일을 checksum 고정 사본으로 포함합니다.

- [`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway),
  commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7` — Sway dotfile 및
  배경화면
- [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes),
  commit `7322626c48be183bfdd7c3eeb2faad1fb69da0f4` —
  `Sweet-Ambar-Blue`, `Qogir-Light`
- [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons),
  commit `1af3af70ccb233bf26f42162f7e65e4a36803667` — `Ars`, `Qogir`,
  `Archcraft` fallback
- [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors),
  commit `8b7e4633cf8e73502f2cfd396d077edf9304c440` — `Sweet`, `Qogirr-Dark`.
  후자의 [대응 공개 소스](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors)도 함께 기록합니다.

복사된 파일은 GPL-3.0을 유지합니다. 소스 잠금, archive hash, 설치 경로는
`module/profile-assets/` 아래에 기록합니다. LinDeX는 Android가 소유하는 전원,
내부 밝기, Bluetooth 동작과 자동 잠금/DPMS를 비활성화하는 문서화된
Debian/chroot 어댑터만 적용하며 시각 파일을 재라이선스하지 않습니다.
Checksum 고정 `lindex-archcraft-sway-public-assets-v2.tar.gz`는 이 공개 입력을
LinDeX가 재현 가능하게 묶은 aggregate이며 Archcraft 업스트림 배포 archive가
아닙니다. Pywal 모드는 설치된 공식 프로필 script
`~/.config/sway/theme/theme.sh --pywal`을 호출합니다. Aggregate에는
Qogirr-Dark 대응 SVG 소스가 `corresponding-source/Qogirr-Dark/src/cursors`
아래에, GPL-3.0 `COPYING` 파일도 함께 실제로 포함됩니다.

Archcraft Sway 제작자의
[Ko-fi 페이지](https://ko-fi.com/s/10f2e87af3)는 감사 표시와 선택적 후원
경로로만 안내합니다. 설치 소스, 필수 결제 또는 라이선스 조건이 아닙니다.
LinDeX는 비공개/유료 Ko-fi archive를 다운로드하거나 포함하지 않습니다.

Wayfire, River, Newm은 현재 릴리스 프로필이 아닙니다. 과거 패키지 목록/provider
실험은 LinDeX 데스크톱에 Archcraft 정체성을 부여하지 않으며 v3에 배포되지
않습니다.

## 상표 및 제휴

LinDeX는 독립 프로젝트 이름입니다. Android, Samsung, Qualcomm, Debian,
Archcraft, 데스크톱 이름과 기타 표장은 각 소유자에게 속합니다. 제휴나 보증을
주장하지 않습니다.
