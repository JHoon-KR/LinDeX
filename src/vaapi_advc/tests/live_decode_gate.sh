#!/bin/sh
set -eu

if [ "$#" -ne 4 ] && [ "$#" -ne 6 ]; then
    echo "usage: $0 DRIVER SOCKET H264_MAIN_30F DECODE_RUNTIME_PROBE [TURNIP_SMOKE H264_MAIN_1IDR]" >&2
    exit 2
fi

driver=$1
socket=$2
sample=$3
probe=$4
turnip_smoke=${5-}
idr_sample=${6-}
driver_dir=$(dirname "$driver")
tmp_dir=$(mktemp -d /tmp/advc-vaapi-live-gate.XXXXXX)
cleanup() {
    rm -f "$tmp_dir/ffmpeg.log" "$tmp_dir/qcom.log" \
        "$tmp_dir/linear-negative.log" "$tmp_dir/linear-positive.json"
    rmdir "$tmp_dir"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

common_env="LIBVA_DRIVER_NAME=advc LIBVA_DRIVERS_PATH=$driver_dir ADVC_VAAPI_ENABLE_AVC=validated-v1 ADVC_VAAPI_SOCKET=$socket"

# A standard libva client must submit the complete sample without falling back.
# The fixture used by the live gate contains exactly 30 decoded pictures.
env $common_env ADVC_VAAPI_QCOM_IMPORT=validated-v1 \
    ffmpeg -hide_banner -loglevel info -hwaccel vaapi \
    -hwaccel_output_format vaapi -i "$sample" -frames:v 30 -f null - \
    >"$tmp_dir/ffmpeg.log" 2>&1
grep -Eq 'frame=[[:space:]]*30([^0-9]|$)' "$tmp_dir/ffmpeg.log"

# Exercise the functions that FFmpeg's null muxer need not force: SyncSurface
# and DRM PRIME export. This consumer was validated for QCOM_COMPRESSED.
env $common_env ADVC_VAAPI_QCOM_IMPORT=validated-v1 \
    "$probe" "$driver" "$sample" >"$tmp_dir/qcom.log" 2>&1
grep -Fq 'decode-prime-pass' "$tmp_dir/qcom.log"
grep -Fq 'fourcc=0x3231564e' "$tmp_dir/qcom.log"
grep -Fq 'modifier=0x0500000000000001' "$tmp_dir/qcom.log"

# LINEAR-only without a linked GPU-repack hook must fail closed. It must not
# silently label the compressed allocation as modifier 0.
if env $common_env ADVC_VAAPI_DECODE_OUTPUT=linear \
    "$probe" "$driver" "$sample" >"$tmp_dir/linear-negative.log" 2>&1; then
    echo "LINEAR negative gate unexpectedly succeeded" >&2
    exit 1
fi

if [ -n "$turnip_smoke" ]; then
    # Positive LINEAR proof is currently supplied by the validated Turnip GPU
    # repack helper. Expected live counters: two outputs (frame + EOS), one AHB
    # frame, one PRIME export, two releases, and an exact modifier-0 descriptor.
    env ADVC_SMOKE_PRIME=1 ADVC_SMOKE_GPU_LINEAR_REPACK=1 \
        "$turnip_smoke" "$socket" video/avc 320 240 "$idr_sample" \
        >"$tmp_dir/linear-positive.json"
    grep -Fq '"ok":true' "$tmp_dir/linear-positive.json"
    grep -Fq '"outputs":2' "$tmp_dir/linear-positive.json"
    grep -Fq '"ahb_outputs":1' "$tmp_dir/linear-positive.json"
    grep -Fq '"prime_exports":1' "$tmp_dir/linear-positive.json"
    grep -Fq '"prime_modifier":0' "$tmp_dir/linear-positive.json"
    grep -Fq '"release_calls":2' "$tmp_dir/linear-positive.json"
    grep -Fq '"prime_status":"turnip-gpu-linear-repack-content-fence-pass"' \
        "$tmp_dir/linear-positive.json"
fi

echo "ADVC VA-API AVC live gate: PASS"
