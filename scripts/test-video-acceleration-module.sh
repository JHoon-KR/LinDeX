#!/bin/sh
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PAYLOAD=$REPO/module/payload/debian/codec
SESSION=$REPO/module/bin/stock-profile-session
LAUNCH=$REPO/module/bin/launch-stock-profile
COMMON=$REPO/module/bin/common.sh
CUSTOMIZE=$REPO/module/customize.sh
VERIFY=$REPO/scripts/verify-v3-module.ps1
INSTALL=$REPO/module/payload/debian/android-drm-install
FIREFOX_LAUNCHER=$REPO/module/payload/debian/lindex-firefox

fail() { echo "video acceleration module test failed: $*" >&2; exit 1; }
require_text() {
    grep -Fq "$1" "$2" || fail "missing '$1' in $2"
}

[ -f "$PAYLOAD/advc_drv_video.so" ] || fail 'VA driver payload missing'
[ -x "$PAYLOAD/advc-repack-gateway" ] || fail 'repack gateway payload missing'
[ -x "$PAYLOAD/advc-vaapi-decode-preflight" ] ||
    fail 'decode capability preflight payload missing'
[ -f "$PAYLOAD/liblindex-firefox-advc-rdd-socket.so" ] ||
    fail 'Firefox RDD socket adapter payload missing'
[ -f "$PAYLOAD/liblindex-firefox-egl-drm-identity.so" ] ||
    fail 'Firefox EGL identity adapter payload missing'
[ -f "$PAYLOAD/SHA256SUMS" ] || fail 'VA driver payload manifest missing'
[ -x "$FIREFOX_LAUNCHER" ] || fail 'scoped Firefox launcher missing or not executable'
(cd "$PAYLOAD" && sha256sum -c SHA256SUMS >/dev/null) ||
    fail 'VA driver payload digest mismatch'
(cd "$REPO/module" && sha256sum -c advc-artifacts.sha256 >/dev/null) ||
    fail 'module ADVC artifact manifest mismatch'
grep -aFq 'advc-codec: destroy before-stop' "$REPO/module/bin/advc-broker" ||
    fail 'release broker lacks before-stop teardown diagnostics'
grep -aFq 'advc-codec: destroy after-stop status=' "$REPO/module/bin/advc-broker" ||
    fail 'release broker lacks after-stop teardown diagnostics'

header=$(od -An -tx1 -N20 "$PAYLOAD/advc_drv_video.so" | tr -d ' \n')
case "$header" in
    7f454c46020101??????????????????????b700) ;;
    *) fail "VA driver is not little-endian AArch64 ELF64: $header" ;;
esac

require_text 'prepare_advc_codec_runtime' "$COMMON"
require_text '[ -c "$ROOTFS/dev/dri/renderD128" ]' "$COMMON"
require_text '[ -e "$ROOTFS/proc/self/fd" ]' "$COMMON"
require_text '[ -d "$ROOTFS/sys/class/drm/renderD128" ]' "$COMMON"
require_text '[ -S "$ROOTFS$ADVC_VAAPI_SOCKET_GUEST" ]' "$COMMON"
require_text 'SESSION_VIDEO_ACCELERATION=disabled' "$LAUNCH"
require_text 'ANDROID_DRM_VIDEO_ACCELERATION="$SESSION_VIDEO_ACCELERATION"' "$LAUNCH"

require_text 'unset LIBVA_DRIVER_NAME LIBVA_DRIVERS_PATH ADVC_VAAPI_SOCKET' "$SESSION"
require_text 'unset GST_VAAPI_DRM_DEVICE' "$SESSION"
require_text 'LIBVA_DRIVER_NAME=advc' "$SESSION"
require_text 'LIBVA_DRIVERS_PATH=$ADVC_VAAPI_DRIVER_DIR' "$SESSION"
require_text 'ADVC_VAAPI_SOCKET=$ADVC_VAAPI_SOCKET_PATH' "$SESSION"
require_text 'GST_VAAPI_DRM_DEVICE=/dev/dri/renderD128' "$SESSION"
require_text 'ADVC_VAAPI_ENABLE_AVC=validated-v1' "$SESSION"
require_text 'ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1' "$SESSION"
require_text 'ADVC_DECODE_PREFLIGHT=$ADVC_CODEC_ROOT/advc-vaapi-decode-preflight' "$SESSION"
require_text 'validated-main-inter-prime-eos-120of120-v1' "$SESSION"
require_text 'validated-profile0-inter-prime-eos-120of120-v1' "$SESSION"
require_text 'unset ANDROID_DRM_CODEC_HEVC_APP_GATE ANDROID_DRM_CODEC_VP9_APP_GATE' "$SESSION"
require_text '"$ADVC_DECODE_PREFLIGHT" "$ADVC_VAAPI_SOCKET_PATH"' "$SESSION"
require_text 'export ADVC_VAAPI_ENABLE_HEVC=$hevc_validation' "$SESSION"
require_text 'export ADVC_VAAPI_ENABLE_VP9=$vp9_validation' "$SESSION"
require_text 'exposure=public-beta' "$SESSION"
require_text 'reason=live-preflight-failed' "$SESSION"
require_text 'ADVC_VAAPI_ENABLE_ENCODE=validated-avc-hevc-v1' "$SESSION"
require_text 'ADVC_REPACK_GATEWAY_ENABLE=validated-qcom-nv12-v1' "$SESSION"
require_text 'ADVC_REPACK_GATEWAY_OUTPUT=auto' "$SESSION"
require_text 'ADVC_VAAPI_DECODE_OUTPUT=auto' "$SESSION"
require_text 'ADVC_VAAPI_ENCODE_INPUT=auto' "$SESSION"
require_text 'ADVC_VAAPI_ASYNC_EXPORT=candidate-firefox-bframe-v1' "$SESSION"
require_text 'LINDEX_FIREFOX_RDD_SOCKET_ACK=firefox-rdd-advc-socket-v1' "$SESSION"
require_text 'LINDEX_EGL_DRM_IDENTITY_ACK=kgsl-card0-renderD128-firefox-glxtest-v1' "$SESSION"
require_text 'export LINDEX_FIREFOX_PRELOAD=$FIREFOX_PRELOAD' "$SESSION"
require_text 'export LD_PRELOAD=$GLES_PRELOAD' "$SESSION"
if grep -Fq 'PRELOAD=$FIREFOX_PRELOAD:$PRELOAD' "$SESSION" ||
   grep -Fq 'PRELOAD=$CODEC_PRELOAD:$PRELOAD' "$SESSION"; then
    fail 'Firefox adapters must not be globally preloaded into the compositor'
fi
require_text 'expected_preload=$egl_adapter:$rdd_adapter' "$FIREFOX_LAUNCHER"
require_text 'export LD_PRELOAD=$expected_preload' "$FIREFOX_LAUNCHER"
require_text 'unset DRM_LEASE_FD DRM_LEASE_LESSEE_ID DRM_LEASE_OBJECTS' "$FIREFOX_LAUNCHER"
require_text 'ADVC_VAAPI_ENABLE_GENERIC_UPLOAD=validated-nv12-v1' "$SESSION"
require_text 'ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1' "$SESSION"
require_text 'GST_VAAPI_ALL_DRIVERS=1' "$SESSION"
require_text 'GST_REGISTRY_FORK=no' "$SESSION"
require_text 'GST_REGISTRY=$XDG_RUNTIME_DIR/lindex-gstreamer-advc-v2.bin' "$SESSION"
require_text 'unset GST_VAAPI_DRM_DEVICE GST_VAAPI_ALL_DRIVERS GST_REGISTRY_FORK GST_REGISTRY' "$SESSION"
require_text 'unset ADVC_VAAPI_ENABLE_WRITE_EXPORT ADVC_VAAPI_ENABLE_GENERIC_UPLOAD' "$SESSION"
require_text 'unset ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS' "$SESSION"
require_text 'unset ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE' "$SESSION"
require_text 'unset ADVC_VAAPI_H264_REORDER_BOUND' "$SESSION"
require_text 'unset ADVC_CODEC_DECODER_DESTROY_DRAIN' "$SESSION"
require_text 'unset ADVC_VAAPI_ENABLE_HEVC ADVC_VAAPI_ENABLE_VP9' "$SESSION"
require_text 'unset ADVC_VAAPI_GPU_LINEAR_REPACK ADVC_VAAPI_DECODE_OUTPUT' "$SESSION"
require_text 'unset ADVC_VAAPI_ENCODE_INPUT ADVC_VAAPI_OUTPUT' "$SESSION"
require_text 'unset ADVC_VAAPI_ASYNC_EXPORT' "$SESSION"
require_text '[ -e /proc/self/fd ]' "$SESSION"
require_text '[ -d /sys/class/drm/renderD128 ]' "$SESSION"
require_text 'sha256sum -c SHA256SUMS' "$SESSION"
if grep -Fq 'export ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS=' "$SESSION"; then
    fail 'private decode EOS must not be exported by the stock session'
fi
if grep -Fq 'export ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE=' "$SESSION"; then
    fail 'unvalidated H.264 in-band config update must not be exported by the stock session'
fi
if grep -Fq 'export ADVC_VAAPI_H264_REORDER_BOUND=' "$SESSION" &&
   ! grep -Fq 'export ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1' "$SESSION"; then
    fail 'H.264 reorder must use the exact validated bounded gate'
fi
if grep -Fq 'export ADVC_CODEC_DECODER_DESTROY_DRAIN=' "$SESSION"; then
    fail 'unvalidated decoder destroy drain must not be exported by the stock session'
fi
if grep -Fq 'export ADVC_VAAPI_QCOM_IMPORT=' "$SESSION"; then
    fail 'Firefox-facing VA driver must reject compressed QCOM descriptors'
fi
require_text 'gstreamer1.0-vaapi' "$INSTALL"
require_text 'gstreamer1.0-plugins-bad' "$INSTALL"
require_text 'gstreamer1.0-tools' "$INSTALL"
require_text 'ffmpeg vainfo' "$INSTALL"

require_text 'payload/debian/codec/advc_drv_video.so' "$CUSTOMIZE"
require_text 'payload/debian/lindex-firefox' "$CUSTOMIZE"
require_text 'usr/local/bin/firefox-esr' "$CUSTOMIZE"
require_text 'opt/android-drm-lease-kit/codec' "$CUSTOMIZE"
require_text 'payload/debian/codec/advc-repack-gateway' "$VERIFY"
require_text 'payload/debian/codec/advc-vaapi-decode-preflight' "$VERIFY"
require_text 'payload/debian/codec/liblindex-firefox-advc-rdd-socket.so' "$VERIFY"
require_text 'payload/debian/codec/advc_drv_video.so' "$VERIFY"
require_text '(?i)(^|/).*smoke[^/]*$' "$VERIFY"
require_text 'advc-ffmpeg-decode-eos-probe' "$VERIFY"
require_text 'advc-nv12-frame-hash' "$VERIFY"

for pattern in '*smoke*' 'advc-ffmpeg-decode-eos-probe*' \
    'advc-nv12-frame-hash'; do
    if find "$REPO/module" -type f -iname "$pattern" -print -quit | grep -q .; then
        fail "release module contains validation-only payload: $pattern"
    fi
done

for script in "$COMMON" "$SESSION" "$LAUNCH" \
    "$REPO/module/bin/auto-service" "$REPO/module/bin/setup-debian-gpu" \
    "$CUSTOMIZE" "$INSTALL" "$FIREFOX_LAUNCHER" \
    "$REPO/scripts/build-advc-vaapi-release.sh" \
    "$REPO/scripts/build-advc-repack-gateway.sh"; do
    sh -n "$script" || fail "syntax error in $script"
done

echo 'video acceleration module integration: PASS'
