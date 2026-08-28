#!/bin/sh
# Static consistency checks for the public application-decode matrix.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
INSTALL=$REPO/module/payload/debian/android-drm-install
SESSION=$REPO/module/bin/stock-profile-session
DRIVER=$REPO/src/vaapi_advc/advc_drv_video.c
SWAY=$REPO/module/profiles/sway.profile
LXQT=$REPO/module/profiles/lxqt.profile
XFCE=$REPO/module/profiles/xfce.profile
DOC=$REPO/docs/VIDEO_APPLICATION_MATRIX.md
KO=$REPO/docs/ko/VIDEO_APPLICATION_MATRIX.md
CODEC_ROOT=$REPO/module/payload/debian/codec
CODEC_MANIFEST=$CODEC_ROOT/SHA256SUMS
DECODE_PREFLIGHT=$CODEC_ROOT/advc-vaapi-decode-preflight

fail() { echo "video application matrix: $*" >&2; exit 1; }
require_text() {
    needle=$1 file=$2
    grep -Fq -- "$needle" "$file" || fail "missing '$needle' in ${file#$REPO/}"
}
reject_text() {
    needle=$1 file=$2
    ! grep -Fq -- "$needle" "$file" || fail "unexpected '$needle' in ${file#$REPO/}"
}

for file in "$INSTALL" "$SESSION" "$DRIVER" "$SWAY" "$LXQT" "$XFCE" \
    "$DOC" "$KO" "$CODEC_MANIFEST" "$DECODE_PREFLIGHT"; do
    [ -f "$file" ] || fail "missing ${file#$REPO/}"
done

# Release artifacts are identified by the immutable codec manifest rather
# than an obsolete compiler-dependent byte count.
require_text 'advc_drv_video.so' "$CODEC_MANIFEST"
require_text 'advc-vaapi-decode-preflight' "$CODEC_MANIFEST"
(cd "$CODEC_ROOT" && sha256sum -c SHA256SUMS >/dev/null) ||
    fail 'codec artifact manifest mismatch'

# Common codec tools are installed, but heavyweight applications are not
# silently added to every desktop profile.
for package in ffmpeg vainfo gstreamer1.0-tools gstreamer1.0-vaapi \
    gstreamer1.0-plugins-bad; do
    require_text "$package" "$INSTALL"
done
for package in vlc firefox-esr chromium; do
    reject_text "$package" "$INSTALL"
    reject_text "$package" "$SWAY"
    reject_text "$package" "$LXQT"
    reject_text "$package" "$XFCE"
done
require_text ' mpv ' "$SWAY"
reject_text ' mpv ' "$LXQT"
reject_text ' mpv ' "$XFCE"

# HEVC Main and VP9 Profile 0 are public-beta capabilities only after the
# session's exact live hardware/PRIME/long-run preflight. A caller-supplied app
# acknowledgement must never manufacture either capability.
require_text 'export LIBVA_DRIVER_NAME=advc' "$SESSION"
require_text 'export ADVC_VAAPI_ENABLE_AVC=validated-v1' "$SESSION"
require_text 'unset ANDROID_DRM_CODEC_HEVC_APP_GATE ANDROID_DRM_CODEC_VP9_APP_GATE' "$SESSION"
reject_text 'HEVC_APP_GATE=' "$SESSION"
reject_text 'VP9_APP_GATE=' "$SESSION"
reject_text 'application-gate-unvalidated' "$SESSION"
reject_text 'validated-hevc-main-display-seek-clean-v1' "$SESSION"
reject_text 'validated-vp9-profile0-display-seek-clean-v1' "$SESSION"
require_text 'reason=live-preflight-failed' "$SESSION"
require_text 'exposure=public-beta' "$SESSION"
require_text 'export ADVC_VAAPI_ENABLE_HEVC=$hevc_validation' "$SESSION"
require_text 'export ADVC_VAAPI_ENABLE_VP9=$vp9_validation' "$SESSION"
require_text 'unset ADVC_VAAPI_ENABLE_HEVC ADVC_VAAPI_ENABLE_VP9' "$SESSION"
require_text 'unset ADVC_VAAPI_GPU_LINEAR_REPACK ADVC_VAAPI_DECODE_OUTPUT' "$SESSION"
require_text 'unset ADVC_VAAPI_ENCODE_INPUT ADVC_VAAPI_OUTPUT' "$SESSION"
require_text 'ADVC_VAAPI_ENABLE_HEVC' "$DRIVER"
require_text 'ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN' "$REPO/src/vaapi_advc/advc_vaapi_policy.h"
require_text 'ADVC_VAAPI_ENABLE_VP9' "$DRIVER"
require_text 'ADVC_VAAPI_VP9_PROFILE0_VALIDATION_TOKEN' "$REPO/src/vaapi_advc/advc_vaapi_policy.h"

# Decode system-memory extraction is intentionally not claimed. DeriveImage is
# encode-only and GetImage remains unimplemented for the whole driver.
derive_block=$(sed -n '/static VAStatus advc_derive_image/,/^}/p' "$DRIVER")
printf '%s\n' "$derive_block" | grep -Fq '!advc_vaapi_encode_owns_surface' ||
    fail 'decode DeriveImage boundary changed without matrix update'
get_block=$(sed -n '/static VAStatus advc_get_image/,/^}/p' "$DRIVER")
printf '%s\n' "$get_block" | grep -Fq 'return unimplemented();' ||
    fail 'GetImage boundary changed without matrix update'

for file in "$DOC" "$KO"; do
    for term in 'FFmpeg' 'mpv' 'VLC' 'Firefox' 'Chromium' 'GStreamer' \
        'vaGetImage' 'vaDeriveImage' 'QCOM UBWC' 'repacked LINEAR' 'EOS'; do
        require_text "$term" "$file"
    done
done
require_text 'public beta' "$DOC"
require_text 'Hardware codec test applications' "$DOC"
require_text '공개 베타' "$KO"
require_text '하드웨어 코덱 테스트 앱' "$KO"
require_text 'SHA256SUMS' "$DOC"
require_text 'SHA256SUMS' "$KO"
require_text 'use_vaapi=false' "$DOC"
require_text 'use_vaapi=false' "$KO"
require_text 'ADVC_VAAPI_ENABLE_HEVC' "$DOC"
require_text 'ADVC_VAAPI_ENABLE_HEVC' "$KO"

echo 'video application matrix: PASS'
