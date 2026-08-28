# 오프라인 패키지

[English](README.md) | [한국어](README.ko.md)

설치 관리자는 일반적으로 고정된 Mesa 압축 파일을 다운로드합니다. 오프라인
모듈을 만들려면 패키징 전에 첫 번째 정확한 파일을 이 디렉터리에 둡니다.

- `mesa-for-android-container_26.2.0-devel-20260709_debian_trixie_arm64.tar.gz`
- `turnip_26.2.0-devel-20260709_debian_trixie_arm64.tar.gz`(선택 사항인
  비패치 Turnip 호환 덮어쓰기)

기본 압축 파일에는 패치된 Turnip Vulkan 드라이버가 이미 포함되어 있으며 이것이
권장 기본값입니다. 두 번째 압축 파일은 단독으로 완전하지 않으므로, LinDeX는 호환
모드를 선택한 경우에만 기본 압축 파일 다음에 덮어씁니다.

Debian 설치 관리자는 오프라인 파일도 checksum으로 검증합니다. 큰 압축 파일은
의도적으로 Git에 commit하지 않습니다.
