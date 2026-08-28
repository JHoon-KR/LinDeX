# 코덱 탐색 및 VA-API 프로필 정책

[English](../CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) | [한국어](CODEC_DISCOVERY_AND_VAAPI_PROFILES.md)

## Android 컴포넌트 탐색

Android capability provider는 고정 컴포넌트 이름이 아니라 정확한 MIME으로
MediaCodec 디코더를 요청합니다. `video/avc`, `video/hevc`,
`video/x-vnd.on2.vp9`, `video/av01`을 탐색하므로 QTI, MediaTek, Exynos,
Rockchip 또는 다른 기기에서 컴포넌트 이름이 달라도 코덱 선택 방식은 바뀌지
않습니다.

반환된 이름은 fail-closed 하드웨어 근거로만 사용합니다. 알려진 Android 및
FFmpeg 소프트웨어 prefix는 software로 분류합니다. secure, software 표시,
중간에 삽입된 prefix, 알 수 없는 이름은 `UNKNOWN`으로 유지하며 hardware로
승격하지 않습니다. 새 vendor에서는 안전한 false negative가 생길 수 있습니다.
새 hardware prefix를 추가하려면 기기 증거와 host test가 필요합니다. 컴포넌트
이름만으로 PRIME export, access-unit 변환, EOS 또는 inter-frame stream 성공을
증명할 수는 없습니다.

NDK r27 media API는 MIME 기반 생성과 선택된 컴포넌트 이름은 제공하지만,
공개 MediaCodecList/profile-level 열거 API는 제공하지 않습니다. 따라서 MIME
탐색 성공을 기기 프로필 지원 주장으로 바꾸지 않으며, 아래 프로필 gate에는
여전히 bounded live 증거가 필요합니다.

## VA-API 광고

`vainfo`, FFmpeg, GStreamer에는 다음 조건의 교집합만 보입니다.

1. broker가 decode 및 PRIME transport를 보고합니다.
2. MediaCodec이 해당 MIME에 대해 non-secure이며 hardware로 분류된
   컴포넌트를 선택했습니다.
3. VA access-unit translator가 정확한 프로필을 구현합니다.
4. 정확한 코덱별 live-validation 환경 gate가 켜져 있습니다.

표준 VA decode로 노출될 수 있는 프로필은 H.264 Constrained Baseline/Main,
HEVC Main, VP9 Profile 0뿐입니다. H.264 Baseline/High, HEVC Main10 및 다른
HEVC 프로필, VP9 Profile 1-3, 알 수 없는 모든 프로필과 모든 AV1 프로필은
프로필 열거, entrypoint query, attribute query, config 생성에서 fail-closed
합니다.

Android가 hardware AV1 decoder를 보고해도 AV1은 의도적으로 숨깁니다. 현재
표준 VA-API AV1 buffer에는 추측 없이 MediaCodec 입력 stream을 구성할 만큼
권위 있는 sequence-header state가 없습니다. MIME 탐색으로 이 semantic gap을
숨겨서는 안 됩니다.

## 남은 live gate

Host test는 정확한 MIME/이름 분류와 VA 프로필 allowlist를 증명하지만 기기를
증명하지는 않습니다. 다른 기기에서 프로필을 켜기 전에 선택된 컴포넌트를
기록하고 해당 프로필로 PRIME export, inter-frame sequence, 명시적 EOS/drain,
surface release, 반복 teardown을 포함한 bounded decode를 통과해야 합니다.
알 수 없는 컴포넌트 이름은 그 증거가 생길 때까지 비활성으로 유지합니다.
탐색 성공만으로 stock application 지원을 주장할 수 없습니다.
