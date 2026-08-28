# 보안 정책

[English](SECURITY.md) | [한국어](SECURITY.ko.md)

## 보고

악용 세부사항이 포함된 공개 issue를 열기 전에 저장소 관리자에게 비공개로
취약점을 보고해 주세요. 기기 serial, 계정 token, Ko-fi cookie, 전체 Android
로그를 포함하지 않습니다.

## 보안 경계

LinDeX는 권한이 있는 DRM 디스크립터, Linux 입력 기기, root 소유 Android
서비스, Debian chroot를 다룹니다. Descriptor 유출, lease 수명주기, 압축 파일
path traversal, 위험한 추출, 명령 injection, WebUI 명령 실행, 의도하지 않은
Android 입력 캡처 관련 보고는 보안 문제입니다.

프로젝트는 Android `system`, `vendor`, `product`를 다시 마운트하거나 수정하는
변경을 지원하지 않습니다. 유료 제3자 자산과 인증정보는 신뢰하는 릴리스 경계
밖입니다.
