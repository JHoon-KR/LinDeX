# LinDeX documentation

[English](README.md) | [한국어](ko/README.md)

The public documentation is maintained as paired English and Korean pages.
English is the default GitHub language; every page below links to its Korean
counterpart.

## Start here

| Topic | English | 한국어 |
|---|---|---|
| Project overview | [README](../README.md) | [README](../README.ko.md) |
| Install, update, remove | [Installation](INSTALLATION.md) | [설치](ko/INSTALLATION.md) |
| WebUI controls | [WebUI](WEBUI.md) | [WebUI](ko/WEBUI.md) |
| Profiles | [Profiles](LINDEX_PROFILES.md) | [프로필](ko/LINDEX_PROFILES.md) |
| Safety and recovery | [Safety](SAFETY.md) | [안전 및 복구](ko/SAFETY.md) |
| Troubleshooting | [Troubleshooting](TROUBLESHOOTING.md) | [문제 해결](ko/TROUBLESHOOTING.md) |

## Design and validation

| Topic | English | 한국어 |
|---|---|---|
| Architecture | [Architecture](ARCHITECTURE.md) | [아키텍처](ko/ARCHITECTURE.md) |
| Vulkan DRM identity | [Vulkan DRM identity](VULKAN_DRM_IDENTITY.md) | [Vulkan DRM identity](ko/VULKAN_DRM_IDENTITY.md) |
| Codec and zero-copy | [Codec status](VIDEO_ZERO_COPY_STATUS.md) | [코덱 상태](ko/VIDEO_ZERO_COPY_STATUS.md) |
| VA-API image modifiers | [Modifier policy](VAAPI_MODIFIER_POLICY.md) | [Modifier 정책](ko/VAAPI_MODIFIER_POLICY.md) |
| Codec discovery and VA profiles | [Discovery policy](CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) | [탐색 정책](ko/CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) |
| Decode failure boundaries | [Failure analysis](DECODE_FAILURE_ANALYSIS.md) | [실패 분석](ko/DECODE_FAILURE_ANALYSIS.md) |
| GStreamer private decode EOS | [Design status](GSTREAMER_PRIVATE_DECODE_EOS.md) | [설계 상태](ko/GSTREAMER_PRIVATE_DECODE_EOS.md) |
| Video application compatibility | [Application matrix](VIDEO_APPLICATION_MATRIX.md) | [앱 매트릭스](ko/VIDEO_APPLICATION_MATRIX.md) |
| Firefox RDD sandbox gateway | [Architecture and acceptance](FIREFOX_RDD_GATEWAY.md) | [구조와 합격 조건](ko/FIREFOX_RDD_GATEWAY.md) |
| USB input routing | [USB input](USB_INPUT_ROUTING.md) | [USB 입력](ko/USB_INPUT_ROUTING.md) |
| Current release evidence | [Validation status](VALIDATION_STATUS.md) | [검증 상태](ko/VALIDATION_STATUS.md) |
| Roadmap | [Roadmap](ROADMAP.md) | [로드맵](ko/ROADMAP.md) |

## Project and release work

| Topic | English | 한국어 |
|---|---|---|
| Release packaging | [Releasing](RELEASING.md) | [릴리스](ko/RELEASING.md) |
| Profile asset packaging | [Profile assets](PROFILE_RUNTIME_PACKAGING.md) | [프로필 자산](ko/PROFILE_RUNTIME_PACKAGING.md) |
| License and attribution | [License and attribution](LICENSE_AND_ATTRIBUTION.md) | [라이선스 및 출처](ko/LICENSE_AND_ATTRIBUTION.md) |
| Project notices | [Notice](../NOTICE.md) | [고지](../NOTICE.ko.md) |
| Third-party components | [Third party](../THIRD_PARTY.md) | [제3자 구성요소](../THIRD_PARTY.ko.md) |
| Contributing | [Contributing](../CONTRIBUTING.md) | [기여](../CONTRIBUTING.ko.md) |
| Security | [Security](../SECURITY.md) | [보안](../SECURITY.ko.md) |
| Changelog | [Changelog](../CHANGELOG.md) | [변경 기록](../CHANGELOG.ko.md) |

## Public scope

The index above is the supported public-documentation surface for LinDeX v3.
Historical device notes, old compositor experiments, local worklogs, generated
fixtures, device logs, and build-tree Markdown are not release documentation
and are excluded by the repository ignore rules.

Source-adjacent protocol specifications and small packaging READMEs remain
developer references in their source directories. They are also paired in
English and Korean, but they are not end-user instructions and do not replace
the user pages above.

### Developer references outside the paired user set

| Reference | English | 한국어 | Classification |
|---|---|---|---|
| ADVC protocol | [English](../src/video_bridge/PROTOCOL.md) | [한국어](../src/video_bridge/PROTOCOL.ko.md) | Normative source-adjacent wire specification; English identifiers are authoritative |
| Offline package inputs | [English](../module/packages/README.md) | [한국어](../module/packages/README.ko.md) | Small packager input note; not an end-user download workflow |

Generated executable and fixture documentation remains outside the public
documentation set. Changes that affect user behavior must also update the
appropriate paired page in this index.

When an English and Korean page differ, treat the English page as normative for
code identifiers and exact command syntax, then open a documentation issue so
both pages can be updated in the same change.
