# LinDeX 문서

[English](../README.md) | [한국어](README.md)

공개 문서는 영문과 한국어 대응 페이지로 관리합니다. GitHub 기본 언어는
영어이며, 아래 모든 페이지에서 한국어 대응본으로 이동할 수 있습니다.

## 시작하기

| 주제 | English | 한국어 |
|---|---|---|
| 프로젝트 개요 | [README](../../README.md) | [README](../../README.ko.md) |
| 설치, 업데이트, 제거 | [Installation](../INSTALLATION.md) | [설치](INSTALLATION.md) |
| WebUI 제어 | [WebUI](../WEBUI.md) | [WebUI](WEBUI.md) |
| 프로필 | [Profiles](../LINDEX_PROFILES.md) | [프로필](LINDEX_PROFILES.md) |
| 안전 및 복구 | [Safety](../SAFETY.md) | [안전 및 복구](SAFETY.md) |
| 문제 해결 | [Troubleshooting](../TROUBLESHOOTING.md) | [문제 해결](TROUBLESHOOTING.md) |

## 설계와 검증

| 주제 | English | 한국어 |
|---|---|---|
| 아키텍처 | [Architecture](../ARCHITECTURE.md) | [아키텍처](ARCHITECTURE.md) |
| Vulkan DRM identity | [Vulkan DRM identity](../VULKAN_DRM_IDENTITY.md) | [Vulkan DRM identity](VULKAN_DRM_IDENTITY.md) |
| 코덱 및 zero-copy | [Codec status](../VIDEO_ZERO_COPY_STATUS.md) | [코덱 상태](VIDEO_ZERO_COPY_STATUS.md) |
| VA-API image modifier | [Modifier policy](../VAAPI_MODIFIER_POLICY.md) | [Modifier 정책](VAAPI_MODIFIER_POLICY.md) |
| 코덱 탐색 및 VA 프로필 | [Discovery policy](../CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) | [탐색 정책](CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) |
| 디코드 실패 경계 | [Failure analysis](../DECODE_FAILURE_ANALYSIS.md) | [실패 분석](DECODE_FAILURE_ANALYSIS.md) |
| GStreamer private decode EOS | [Design status](../GSTREAMER_PRIVATE_DECODE_EOS.md) | [설계 상태](GSTREAMER_PRIVATE_DECODE_EOS.md) |
| 비디오 앱 호환성 | [Application matrix](../VIDEO_APPLICATION_MATRIX.md) | [앱 매트릭스](VIDEO_APPLICATION_MATRIX.md) |
| Firefox RDD 샌드박스 게이트웨이 | [Architecture and acceptance](../FIREFOX_RDD_GATEWAY.md) | [구조와 합격 조건](FIREFOX_RDD_GATEWAY.md) |
| USB 입력 라우팅 | [USB input](../USB_INPUT_ROUTING.md) | [USB 입력](USB_INPUT_ROUTING.md) |
| 현재 릴리스 증거 | [Validation status](../VALIDATION_STATUS.md) | [검증 상태](VALIDATION_STATUS.md) |
| 로드맵 | [Roadmap](../ROADMAP.md) | [로드맵](ROADMAP.md) |

## 프로젝트와 릴리스 작업

| 주제 | English | 한국어 |
|---|---|---|
| 릴리스 패키징 | [Releasing](../RELEASING.md) | [릴리스](RELEASING.md) |
| 프로필 자산 패키징 | [Profile assets](../PROFILE_RUNTIME_PACKAGING.md) | [프로필 자산](PROFILE_RUNTIME_PACKAGING.md) |
| 라이선스 및 출처 | [License and attribution](../LICENSE_AND_ATTRIBUTION.md) | [라이선스 및 출처](LICENSE_AND_ATTRIBUTION.md) |
| 프로젝트 고지 | [Notice](../../NOTICE.md) | [고지](../../NOTICE.ko.md) |
| 제3자 구성요소 | [Third party](../../THIRD_PARTY.md) | [제3자 구성요소](../../THIRD_PARTY.ko.md) |
| 기여 | [Contributing](../../CONTRIBUTING.md) | [기여](../../CONTRIBUTING.ko.md) |
| 보안 | [Security](../../SECURITY.md) | [보안](../../SECURITY.ko.md) |
| 변경 기록 | [Changelog](../../CHANGELOG.md) | [변경 기록](../../CHANGELOG.ko.md) |

## 공개 범위

위 인덱스가 LinDeX v3에서 지원하는 공개 문서 범위입니다. 과거 기기 기록,
이전 컴포지터 실험, 로컬 작업 로그, 자동 생성 fixture, 기기 로그, 빌드 트리의
Markdown은 릴리스 문서가 아니며 저장소 ignore 규칙으로 제외합니다.

소스 디렉터리 옆의 프로토콜 명세와 짧은 패키징 README는 개발자 참고자료로
남고 영문/한국어 대응본도 제공합니다. 사용자 안내가 아니며 위의 사용자 문서를
대체하지 않습니다.

### 대응 사용자 문서 밖의 개발자 참고자료

| 참고자료 | English | 한국어 | 분류 |
|---|---|---|---|
| ADVC 프로토콜 | [English](../../src/video_bridge/PROTOCOL.md) | [한국어](../../src/video_bridge/PROTOCOL.ko.md) | 소스 옆의 normative wire 명세이며 영문 식별자가 기준 |
| 오프라인 패키지 입력 | [English](../../module/packages/README.md) | [한국어](../../module/packages/README.ko.md) | 작은 패키저 입력 기록이며 최종 사용자 다운로드 절차가 아님 |

자동 생성 실행 파일 및 fixture 문서는 공개 문서 범위 밖에 둡니다. 사용자 동작에
영향을 주는 변경은 이 인덱스의 적절한 대응 페이지도 갱신해야 합니다.

영문과 한국어 페이지 내용이 다르면 코드 식별자와 정확한 명령 구문은 영문을
기준으로 판단하고, 같은 변경에서 두 페이지를 함께 갱신할 수 있도록 문서 이슈를
열어 주세요.
