# 프로필 자산 패키징

[English](../PROFILE_RUNTIME_PACKAGING.md) | [한국어](PROFILE_RUNTIME_PACKAGING.md)

이 페이지는 릴리스 엔지니어링용입니다. 현재 릴리스는 Archcraft Sway Free,
LXQt + stock labwc, XFCE + stock labwc의 3개 프로필만 제공합니다. Wayfire,
River, Newm provider 런타임은 배포하지 않습니다.

## 현재 패키지 자산

LXQt와 XFCE는 서명된 Debian Trixie 패키지를 사용하며 별도 프로필 자산
archive가 필요하지 않습니다. Archcraft Sway Free에는 checksum으로 고정된
다음 두 archive가 필요합니다.

```text
archcraft-sway-free-e4d0126d.tar.gz
lindex-archcraft-sway-public-assets-v2.tar.gz
```

첫 번째는 공식 공개 GPL-3.0
[`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway)
저장소 commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`의 전체
archive입니다. 고정 SHA-256은
`da89184c13bb68affc89b2638efb2d93736dc685e4104f6a5a497c6f9e43dadc`입니다.

두 번째 파일은 Archcraft 업스트림 배포 archive가 아닙니다. LinDeX가 다음의
고정된 공개 GPL-3.0 source 하위 트리만 재현 가능하게 묶은 aggregate입니다.

- [`archcraft-os/archcraft-themes`](https://github.com/archcraft-os/archcraft-themes)
  commit `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`의
  `Sweet-Ambar-Blue`;
- [`archcraft-os/archcraft-icons`](https://github.com/archcraft-os/archcraft-icons)
  commit `1af3af70ccb233bf26f42162f7e65e4a36803667`의 `Ars`;
- [`archcraft-os/archcraft-cursors`](https://github.com/archcraft-os/archcraft-cursors)
  commit `8b7e4633cf8e73502f2cfd396d077edf9304c440`의 `Sweet`;
- `archcraft-themes` commit
  `7322626c48be183bfdd7c3eeb2faad1fb69da0f4`의 `Qogir-Light`;
- `archcraft-icons` commit
  `1af3af70ccb233bf26f42162f7e65e4a36803667`의 `Qogir` 및 `Archcraft`
  fallback;
- `archcraft-cursors` commit
  `8b7e4633cf8e73502f2cfd396d077edf9304c440`의 `Qogirr-Dark`와
  [대응 공개 cursor 소스](https://github.com/vinceliuice/Qogir-icon-theme/tree/488945d0e8c95ed9ce4108b65116845d15b9602f/src/cursors).

고정 SHA-256은
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`입니다.
정확한 source path와 install path는
`module/profile-assets/APPEARANCE_SOURCES.lock`에 기록합니다.
Aggregate에는 Qogirr-Dark 대응 SVG 소스
`corresponding-source/Qogirr-Dark/src/cursors`와 GPL-3.0 `COPYING` 파일
`corresponding-source/Qogirr-Dark/COPYING`이 실제로 포함됩니다.

## 허용 계약

릴리스 패키지와 설치 관리자는 두 archive, 인접 `.sha256` 파일, source lock을
모두 요구합니다. Archive 누락, digest 불일치, 위험한 archive 경로, symlink
escape, 특수 파일, 불완전한 dotfile 트리, source-lock drift를 거부합니다. 설치된
source marker가 예상 commit, archive digest, GPL-3.0 라이선스, Debian 어댑터
revision을 기록하기 전에는 Sway 프로필이 ready가 될 수 없습니다.

이 자산은 검토된 공개 GitHub 자산이며 Ko-fi 파일이 아닙니다. 릴리스 작업에서
유료/비공개 archive로 대체하거나 사용자 다운로드 폴더를 검색하거나 자산 검증
실패 시 단순 fallback Sway 프로필을 조용히 생성하면 안 됩니다. 제작자의
[Ko-fi 페이지](https://ko-fi.com/s/10f2e87af3)는 선택적 후원 링크일 뿐이며 설치
소스, 필수 결제, 라이선스 조건이 아닙니다.

## 설치 경계

Dotfile은 선택한 사용자의 Sway 설정 아래에 설치합니다. LinDeX aggregate는
정확히 고정된 Dark/Light GTK 테마와 아이콘/커서 트리를 `/usr/share` 아래에
설치하고 출처 자료는 `/usr/share/doc/lindex`에 보존합니다. WebUI 빠른 시작은
공식 Dark, 공식 Light, Pywal 중 하나를 적용합니다. Pywal은 설치된 공식 script
경로 `~/.config/sway/theme/theme.sh --pywal`을 사용하며 추적하지 않은 자산
소스를 추가하지 않습니다. 이후 LinDeX는 문서화된 Debian/chroot 안전 어댑터만
적용합니다. 전원 동작은 로그아웃으로 제한하고 내부 밝기와 Bluetooth 바인딩을
비활성화하며 leased 외부 출력의 자동 잠금/DPMS를 끕니다.

이 프로필 자산 경로는 private wlroots, Wayland, libdrm, River, Newm 런타임
스택을 포함하지 않습니다. 개발 트리에 남아 있는 과거 provider builder 자료는
역사적 기록이며 v3 릴리스 입력으로 취급하면 안 됩니다.
