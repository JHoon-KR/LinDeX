#!/bin/sh
set -eu

# Guest-side half of the isolated live codec matrix.  Run exactly one case
# against a fresh broker.  The Android-side runner creates and later removes
# the socket mount, broker PID and all transient files.

usage() {
    cat <<'EOF'
Usage: test-advc-isolated-decode-case.sh \
  --case CASE --driver DRIVER --socket SOCKET --broker-log LOG \
  --eos-probe PREBUILT_SO --gstreamer-diagnostic SCRIPT \
  --pixel-hash-tool PREBUILT_ARM64_TOOL \
  --report-dir NEW_DIR

CASE is one of:
  h264-baseline       Four-slice IDR -> P plus PRIME/release/EOS
  hevc-narrow         Restricted one-IRAP HEVC Main success candidate
  hevc-main-inter     Four-frame HEVC Main inter-frame transport candidate
  hevc-main-pixel-accuracy
                      Four-frame HEVC Main I/P/B POC-keyed NV12 comparison
  vp9-profile0        VP9 Profile 0 key -> inter plus PRIME/release/EOS
  gstreamer-h264-vasurface-eos-teardown
                      Require clean bus EOS and decoder teardown without drain
  encode-h264-hevc-ffmpeg-gstreamer
                      FFmpeg 30-frame and GStreamer 5-frame AVC/HEVC encode

All input paths must live below /tmp/lindex-codec-live.*. The production
driver and production broker socket are rejected deliberately.
EOF
}

fail() {
    if [ -n "${report_dir:-}" ] && [ -d "$report_dir" ]; then
        reason=$(printf '%s' "$*" | sed 's/\\/\\\\/g; s/"/\\"/g')
        printf '{"schema":"lindex-isolated-decode-verdict-v1","case":"%s","pass":false,"reason":"%s"}\n' \
            "${case_name:-unknown}" "$reason" >"$report_dir/verdict.json"
    fi
    printf 'isolated decode case failed: %s\n' "$*" >&2
    exit 1
}

write_pass_verdict() {
    classification=$1
    shift
    {
        printf '{"schema":"lindex-isolated-decode-verdict-v1","case":"%s","pass":true,"classification":"%s"' \
            "$case_name" "$classification"
        while [ "$#" -ge 2 ]; do
            printf ',"%s":%s' "$1" "$2"
            shift 2
        done
        printf '}\n'
    } >"$report_dir/verdict.json"
}

case_name=
driver=
socket_path=
broker_log=
eos_probe=
gstreamer_diagnostic=
pixel_hash_tool=
report_dir=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --case) [ "$#" -ge 2 ] || fail '--case needs a value'; case_name=$2; shift 2 ;;
        --driver) [ "$#" -ge 2 ] || fail '--driver needs a value'; driver=$2; shift 2 ;;
        --socket) [ "$#" -ge 2 ] || fail '--socket needs a value'; socket_path=$2; shift 2 ;;
        --broker-log) [ "$#" -ge 2 ] || fail '--broker-log needs a value'; broker_log=$2; shift 2 ;;
        --eos-probe) [ "$#" -ge 2 ] || fail '--eos-probe needs a value'; eos_probe=$2; shift 2 ;;
        --gstreamer-diagnostic) [ "$#" -ge 2 ] || fail '--gstreamer-diagnostic needs a value'; gstreamer_diagnostic=$2; shift 2 ;;
        --pixel-hash-tool) [ "$#" -ge 2 ] || fail '--pixel-hash-tool needs a value'; pixel_hash_tool=$2; shift 2 ;;
        --report-dir) [ "$#" -ge 2 ] || fail '--report-dir needs a value'; report_dir=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

case "$case_name" in
    h264-baseline|hevc-narrow|hevc-main-inter|hevc-main-pixel-accuracy|vp9-profile0|\
gstreamer-h264-vasurface-eos-teardown|encode-h264-hevc-ffmpeg-gstreamer) ;;
    '') usage >&2; exit 2 ;;
    *) fail "unknown case: $case_name" ;;
esac

for path in "$driver" "$socket_path" "$broker_log" "$eos_probe" \
    "$gstreamer_diagnostic"; do
    case "$path" in
        /tmp/lindex-codec-live.*/*) ;;
        *) fail "non-transient path refused: $path" ;;
    esac
done
case "$report_dir" in
    /tmp/lindex-codec-live.*/*) ;;
    *) fail "non-transient report directory refused: $report_dir" ;;
esac
case "$socket_path" in
    /run/android-drm/*|*/advc-broker-1.1.sock)
        fail 'production broker socket refused'
        ;;
esac

[ -f "$driver" ] && [ -r "$driver" ] || fail "driver is not readable: $driver"
[ -S "$socket_path" ] || fail "isolated broker socket is missing: $socket_path"
[ -f "$broker_log" ] || fail "broker log is missing: $broker_log"
case "$case_name" in
    h264-baseline|hevc-narrow|hevc-main-inter|vp9-profile0)
        [ -f "$eos_probe" ] && [ -r "$eos_probe" ] ||
            fail "prebuilt ARM64 EOS probe is missing: $eos_probe"
        ;;
    gstreamer-h264-vasurface-eos-teardown)
        [ -f "$gstreamer_diagnostic" ] && [ -x "$gstreamer_diagnostic" ] ||
            fail "GStreamer diagnostic is not executable: $gstreamer_diagnostic"
        ;;
    encode-h264-hevc-ffmpeg-gstreamer) ;;
    hevc-main-pixel-accuracy)
        [ -f "$eos_probe" ] && [ -r "$eos_probe" ] ||
            fail "prebuilt ARM64 EOS probe is missing: $eos_probe"
        [ -f "$pixel_hash_tool" ] && [ -x "$pixel_hash_tool" ] ||
            fail "prebuilt ARM64 NV12 hash tool is missing: $pixel_hash_tool"
        ;;
esac
[ ! -e "$report_dir" ] || fail "report directory already exists: $report_dir"
mkdir -m 0700 "$report_dir"

for tool in ffmpeg ffprobe gst-launch-1.0 timeout grep sed awk wc mktemp sort cmp sha256sum tr; do
    command -v "$tool" >/dev/null 2>&1 || fail "required command is missing: $tool"
done
umask 077
task_dir=$(mktemp -d /tmp/lindex-codec-case.XXXXXX)
cleanup() {
    case "$task_dir" in
        /tmp/lindex-codec-case.*) rm -rf -- "$task_dir" ;;
    esac
}
trap cleanup EXIT HUP INT TERM

driver_dir=$(dirname -- "$driver")
ffmpeg_log=$report_dir/ffmpeg.log
summary=$report_dir/summary.txt
fixture=$task_dir/input.bitstream
hook=$eos_probe

common_decode_env="LIBVA_DRIVER_NAME=advc LIBVA_DRIVERS_PATH=$driver_dir ADVC_VAAPI_SOCKET=$socket_path ADVC_VAAPI_DECODE_OUTPUT=qcom ADVC_VAAPI_QCOM_IMPORT=validated-v1 ADVC_VAAPI_TRACE=1 XDG_CACHE_HOME=$task_dir/cache XDG_CONFIG_HOME=$task_dir/config"

generate_h264() {
    timeout -k 2 20 ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i 'testsrc2=size=320x240:rate=2' -frames:v 4 \
        -c:v libx264 -pix_fmt yuv420p -profile:v main -threads 1 \
        -x264-params 'keyint=30:min-keyint=30:scenecut=0:bframes=0:ref=2:slices=4:repeat-headers=1:aud=1' \
        -f h264 "$fixture"
}

generate_hevc_narrow() {
    hevc_full=$task_dir/hevc-main-two-frame.bitstream
    timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i 'testsrc2=size=320x240:rate=1' -frames:v 2 \
        -c:v libx265 -pix_fmt yuv420p -profile:v main -threads 1 \
        -x265-params 'keyint=30:min-keyint=30:scenecut=0:bframes=0:ref=1:repeat-headers=1:aud=1:frame-threads=1:wpp=0:no-sao=1' \
        -f hevc "$hevc_full"
    # A one-frame encode is tagged Main Intra by x265 and is correctly rejected
    # by the driver's Main-only profile gate. Encode a real Main GOP, then copy
    # only its first IRAP access unit for the intentionally narrow live case.
    timeout -k 2 20 ffmpeg -nostdin -hide_banner -loglevel error \
        -f hevc -i "$hevc_full" -map 0:v:0 -frames:v 1 -c:v copy \
        -f hevc "$fixture"
}

generate_hevc_normal() {
    timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i 'testsrc2=size=320x240:rate=4' -frames:v 4 \
        -c:v libx265 -pix_fmt yuv420p -profile:v main -threads 1 \
        -x265-params 'keyint=30:min-keyint=30:scenecut=0:bframes=2:ref=3:repeat-headers=1:frame-threads=1:wpp=0' \
        -f hevc "$fixture"
}

generate_vp9() {
    timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel error \
        -f lavfi -i 'testsrc2=size=320x240:rate=2' -frames:v 2 \
        -c:v libvpx-vp9 -pix_fmt yuv420p -profile:v 0 -threads 1 \
        -deadline realtime -cpu-used 8 -lag-in-frames 0 -auto-alt-ref 0 \
        -g 30 -f ivf "$fixture"
}

run_decode() {
    codec_gate=$1
    use_hook=$2
    inband_gate=$3
    preload=
    private_eos=
    if [ "$use_hook" = yes ]; then
        preload=$hook
        private_eos=validated-signal-progress-v1
    fi
    set +e
    # shellcheck disable=SC2086
    env $common_decode_env \
        ADVC_VAAPI_ENABLE_AVC="$codec_gate" \
        ADVC_VAAPI_ENABLE_HEVC="$codec_gate" \
        ADVC_VAAPI_ENABLE_VP9="$codec_gate" \
        ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE="$inband_gate" \
        ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS="$private_eos" \
        LD_PRELOAD="$preload" \
        timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel verbose \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$fixture" \
        -frames:v 16 -f null - >"$ffmpeg_log" 2>&1
    decode_status=$?
    set -e
}

run_hevc_pixel_accuracy() {
    software_md5=$report_dir/hevc-software.framemd5
    software_raw=$report_dir/hevc-software.nv12
    software_by_pts=$report_dir/hevc-software.by-pts
    hardware_by_pts=$report_dir/hevc-hardware.by-pts

    timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel error \
        -f hevc -i "$fixture" -frames:v 4 -pix_fmt nv12 \
        -f framemd5 "$software_md5"
    timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel error \
        -f hevc -i "$fixture" -frames:v 4 -pix_fmt nv12 \
        -f rawvideo "$software_raw"
    timeout -k 2 10 "$pixel_hash_tool" "$software_raw" 320 240 \
        >"$software_by_pts"

    set +e
    # Compare actual pixels, not only successful surface export.  The private
    # EOS probe drains delayed HEVC B frames, exports PRIME2, and uses the
    # validation-only Turnip consumer to read back the logical NV12 crop.
    # shellcheck disable=SC2086
    env $common_decode_env \
        ADVC_VAAPI_ENABLE_HEVC=validated-main-v1 \
        ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS=validated-signal-progress-v1 \
        ADVC_EOS_PROBE_PIXEL_HASH=validated-turnip-poc-v1 \
        LD_PRELOAD="$hook" \
        timeout -k 2 30 ffmpeg -nostdin -hide_banner -loglevel verbose \
        -f hevc -hwaccel vaapi -hwaccel_output_format vaapi -i "$fixture" \
        -frames:v 4 -f null - >"$ffmpeg_log" 2>&1
    decode_status=$?
    set -e
    [ "$decode_status" -eq 0 ] ||
        fail "HEVC pixel-accuracy hardware decode exited $decode_status"

    sed -n 's/.*pixel-hash-pass .*poc=\(-\{0,1\}[0-9][0-9]*\) .*fnv1a64=\([0-9a-f][0-9a-f]*\) bytes=\([0-9][0-9]*\).*/poc=\1 fnv1a64=\2 bytes=\3/p' \
        "$ffmpeg_log" | sort -n -t= -k2,2 >"$hardware_by_pts"
    for output in "$software_by_pts" "$hardware_by_pts"; do
        lines=$(wc -l <"$output" | tr -d '[:space:]')
        unique_pts=$(sed -n 's/^poc=\([^ ]*\).*/\1/p' "$output" |
            sort -n -u | wc -l | tr -d '[:space:]')
        [ "$lines" -eq 4 ] || fail "HEVC pixel count was $lines, expected 4"
        [ "$unique_pts" -eq 4 ] || fail 'HEVC POC set was not four unique pictures'
    done
    cmp "$software_by_pts" "$hardware_by_pts" >/dev/null ||
        fail 'HEVC POC-keyed NV12 pixels differ from software decode'
    pixel_set_sha=$(sha256sum "$hardware_by_pts" | awk '{ print $1 }')
}

run_gstreamer_diagnostic() {
    [ -c /dev/dri/renderD128 ] ||
        fail 'GStreamer ADVC render node is missing: /dev/dri/renderD128'
    [ -e /proc/self/fd ] || fail 'GStreamer ADVC /proc/self/fd mount is missing'
    [ -d /sys/class/drm/renderD128 ] ||
        fail 'GStreamer ADVC sysfs render node is missing: /sys/class/drm/renderD128'
    gstreamer_timeout=${ADVC_GSTREAMER_TIMEOUT:-10}
    case "$gstreamer_timeout" in
        ''|*[!0-9]*) fail 'ADVC_GSTREAMER_TIMEOUT must be an integer from 1 to 60' ;;
    esac
    [ "$gstreamer_timeout" -ge 1 ] && [ "$gstreamer_timeout" -le 60 ] ||
        fail 'ADVC_GSTREAMER_TIMEOUT must be an integer from 1 to 60'
    set +e
    # shellcheck disable=SC2086
    env $common_decode_env \
        ADVC_VAAPI_ENABLE_AVC=validated-v1 \
        GST_VAAPI_DRM_DEVICE=/dev/dri/renderD128 \
        GST_VAAPI_ALL_DRIVERS=1 \
        "$gstreamer_diagnostic" --timeout "$gstreamer_timeout" \
        --output-dir "$report_dir/gstreamer" "$fixture" \
        >"$report_dir/gstreamer.stdout" 2>&1
    decode_status=$?
    set -e
}

run_encode_regression() {
    [ -c /dev/dri/renderD128 ] ||
        fail 'ADVC encode render node is missing: /dev/dri/renderD128'
    [ -e /proc/self/fd ] || fail 'ADVC encode /proc/self/fd mount is missing'
    [ -d /sys/class/drm/renderD128 ] ||
        fail 'ADVC encode sysfs render node is missing'
    encode_env="LIBVA_DRIVER_NAME=advc LIBVA_DRIVERS_PATH=$driver_dir ADVC_VAAPI_SOCKET=$socket_path ADVC_VAAPI_ENABLE_AVC=validated-v1 ADVC_VAAPI_ENABLE_ENCODE=validated-avc-hevc-v1 ADVC_VAAPI_ENABLE_GENERIC_UPLOAD=validated-nv12-v1 ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1 ADVC_VAAPI_TRACE=1 HOME=$task_dir/home XDG_RUNTIME_DIR=$task_dir/runtime XDG_CACHE_HOME=$task_dir/cache XDG_CONFIG_HOME=$task_dir/config"
    mkdir -m 0700 "$task_dir/home" "$task_dir/runtime" "$task_dir/cache" "$task_dir/config"

    for codec in h264 hevc; do
        encoder=${codec}_vaapi
        profile=main
        [ "$codec" = h264 ] && profile=constrained_baseline
        output=$report_dir/ffmpeg-$codec.bin
        log=$report_dir/ffmpeg-$codec.log
        decoded=$report_dir/ffmpeg-$codec.framemd5
        # shellcheck disable=SC2086
        env $encode_env timeout -k 2 30 ffmpeg -nostdin -hide_banner \
            -loglevel verbose -y \
            -init_hw_device vaapi=advc:/dev/dri/renderD128 \
            -filter_hw_device advc \
            -f lavfi -i testsrc2=size=320x240:rate=30:duration=1 \
            -vf format=nv12,hwupload -frames:v 30 -an \
            -c:v "$encoder" -profile:v "$profile" -rc_mode VBR -b:v 2000k \
            -bf 0 -g 30 -f "$codec" "$output" >"$log" 2>&1 ||
            fail "FFmpeg $codec encode failed"
        [ -s "$output" ] || fail "FFmpeg $codec output is empty"
        ffprobe -v error -f "$codec" -count_frames \
            -show_entries stream=codec_name,profile,width,height,pix_fmt,nb_read_frames \
            -of compact "$output" >"$report_dir/ffmpeg-$codec.ffprobe"
        ffmpeg -nostdin -hide_banner -loglevel error -f "$codec" -i "$output" \
            -map 0:v:0 -f framemd5 "$decoded"
        eval ffmpeg_${codec}_frames=$(grep -c '^[0-9]' "$decoded")
        eval frame_count=\$ffmpeg_${codec}_frames
        [ "$frame_count" -eq 30 ] ||
            fail "FFmpeg $codec software verification decoded $frame_count frames"
        traces=$(count_pattern 'advc-vaapi: begin-picture route=encode' "$log")
        [ "$traces" -ge 30 ] ||
            fail "FFmpeg $codec did not prove 30 ADVC encode submissions"
    done

    for codec in h264 h265; do
        element=vaapi${codec}enc
        demux=$codec
        caps=video/x-h264,stream-format=byte-stream,alignment=au
        result_name=h264
        if [ "$codec" = h265 ]; then
            demux=hevc
            caps=video/x-h265,stream-format=byte-stream,alignment=au
            result_name=hevc
        fi
        output=$report_dir/gstreamer-$result_name.bin
        log=$report_dir/gstreamer-$result_name.log
        decoded=$report_dir/gstreamer-$result_name.framemd5
        registry=$task_dir/gst-registry-$result_name.bin
        # shellcheck disable=SC2086
        env $encode_env GST_VAAPI_ALL_DRIVERS=1 \
            GST_VAAPI_DRM_DEVICE=/dev/dri/renderD128 GST_REGISTRY_FORK=no \
            GST_REGISTRY="$registry" timeout -k 2 30 gst-launch-1.0 -e \
            videotestsrc num-buffers=5 is-live=false ! \
            video/x-raw,width=320,height=240,framerate=30/1 ! \
            videoconvert ! video/x-raw,format=NV12 ! \
            "$element" rate-control=vbr bitrate=2000 keyframe-period=30 ! \
            "$caps" ! filesink location="$output" >"$log" 2>&1 ||
            fail "GStreamer $result_name encode failed"
        [ -s "$output" ] || fail "GStreamer $result_name output is empty"
        ffprobe -v error -f "$demux" -count_frames \
            -show_entries stream=codec_name,profile,width,height,pix_fmt,nb_read_frames \
            -of compact "$output" >"$report_dir/gstreamer-$result_name.ffprobe"
        ffmpeg -nostdin -hide_banner -loglevel error -f "$demux" -i "$output" \
            -map 0:v:0 -f framemd5 "$decoded"
        eval gstreamer_${result_name}_frames=$(grep -c '^[0-9]' "$decoded")
        eval frame_count=\$gstreamer_${result_name}_frames
        [ "$frame_count" -eq 5 ] ||
            fail "GStreamer $result_name software verification decoded $frame_count frames"
    done
}

count_pattern() {
    pattern=$1
    file=$2
    grep -Ec -- "$pattern" "$file" 2>/dev/null || true
}

wait_for_broker_pattern() {
    pattern=$1
    attempts=$2
    attempt=0
    while [ "$attempt" -lt "$attempts" ]; do
        grep -Eq -- "$pattern" "$broker_log" 2>/dev/null && return 0
        attempt=$((attempt + 1))
        sleep 0.05
    done
    return 1
}

write_queue_evidence() {
    {
        printf 'broker_queue_evidence:\n'
        grep -E 'advc-codec: (started|queue )' "$broker_log" || true
    } >>"$summary"
}

case "$case_name" in
    h264-baseline)
        generate_h264
        run_decode validated-v1 yes ''
        [ "$decode_status" -eq 0 ] || fail "H.264 FFmpeg decode exited $decode_status"
        grep -Eq 'frame=[[:space:]]*4([^0-9]|$)' "$ffmpeg_log" ||
            fail 'H.264 decode did not complete four frames'
        slice_traces=$(count_pattern 'advc-vaapi: slices=4 ' "$ffmpeg_log")
        midstream_csd=$(count_pattern 'advc-codec: queue .*flags=0x4 ' "$broker_log")
        [ "$slice_traces" -ge 4 ] || fail "expected four four-slice pictures, saw $slice_traces"
        [ "$midstream_csd" -eq 0 ] || fail 'H.264 unexpectedly queued mid-stream CODEC_CONFIG'
        ! grep -Fq 'advc-vaapi: in-band-codec-config' "$ffmpeg_log" ||
            fail 'H.264 unexpectedly required the in-band config candidate'
        grep -Fq 'lindex-eos-probe: eos-pass' "$ffmpeg_log" || fail 'H.264 EOS did not complete'
        grep -Eq 'lindex-eos-probe: context-summary .*submitted=4 .*eos=complete' "$ffmpeg_log" ||
            fail 'H.264 probe did not observe exactly four pictures and EOS'
        prime_exports=$(count_pattern 'lindex-eos-probe: prime-sync-pass' "$ffmpeg_log")
        [ "$prime_exports" -ge 4 ] || fail "H.264 PRIME exports were incomplete: $prime_exports"
        grep -Fq 'lindex-eos-probe: surface-release-pass' "$ffmpeg_log" ||
            fail 'H.264 surface release lifecycle missing'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=h264-main-four-slice-prime-eos-pass\n'
            printf 'ffmpeg_exit=%s\n' "$decode_status"
            printf 'four_slice_pictures=%s\n' "$slice_traces"
            printf 'midstream_codec_config_queues=%s\n' "$midstream_csd"
            printf 'prime_exports=%s\n' "$prime_exports"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict h264-main-four-slice-prime-eos-pass \
            ffmpegExit "$decode_status" fourSlicePictures "$slice_traces" \
            midstreamCodecConfigQueues "$midstream_csd" \
            primeExports "$prime_exports"
        ;;
    hevc-narrow)
        generate_hevc_narrow
        run_decode validated-main-v1 yes ''
        [ "$decode_status" -eq 0 ] || fail "narrow HEVC decode exited $decode_status"
        grep -Eq 'frame=[[:space:]]*1([^0-9]|$)' "$ffmpeg_log" ||
            fail 'narrow HEVC did not complete one frame'
        grep -Eq 'lindex-eos-probe: context-summary .*submitted=1 .*eos=complete' "$ffmpeg_log" ||
            fail 'narrow HEVC probe did not observe one picture and EOS'
        prime_exports=$(count_pattern 'lindex-eos-probe: prime-sync-pass' "$ffmpeg_log")
        [ "$prime_exports" -ge 1 ] || fail 'narrow HEVC PRIME export missing'
        grep -Fq 'lindex-eos-probe: surface-release-pass' "$ffmpeg_log" ||
            fail 'narrow HEVC surface release lifecycle missing'
        grep -Fq 'lindex-eos-probe: eos-pass' "$ffmpeg_log" || fail 'narrow HEVC EOS missing'
        ! grep -Fq 'hevc-codec-config failed' "$ffmpeg_log" || fail 'narrow HEVC translator failed closed unexpectedly'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=restricted-hevc-main-pass\n'
            printf 'ffmpeg_exit=%s\n' "$decode_status"
            printf 'prime_exports=%s\n' "$prime_exports"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict restricted-hevc-main-pass \
            ffmpegExit "$decode_status" primeExports "$prime_exports"
        ;;
    hevc-main-inter)
        generate_hevc_normal
        run_decode validated-main-v1 yes ''
        [ "$decode_status" -eq 0 ] || fail "HEVC Main inter decode exited $decode_status"
        grep -Eq 'frame=[[:space:]]*4([^0-9]|$)' "$ffmpeg_log" ||
            fail 'HEVC Main inter decode did not complete four frames'
        grep -Eq 'lindex-eos-probe: context-summary .*submitted=4 .*eos=complete' "$ffmpeg_log" ||
            fail 'HEVC Main inter probe did not observe four pictures and EOS'
        prime_exports=$(count_pattern 'lindex-eos-probe: prime-sync-pass' "$ffmpeg_log")
        [ "$prime_exports" -ge 4 ] || fail "HEVC Main inter PRIME exports were incomplete: $prime_exports"
        grep -Fq 'lindex-eos-probe: surface-release-pass' "$ffmpeg_log" ||
            fail 'HEVC Main inter surface release lifecycle missing'
        grep -Fq 'lindex-eos-probe: eos-pass' "$ffmpeg_log" ||
            fail 'HEVC Main inter EOS lifecycle missing'
        ! grep -Fq 'advc-vaapi: hevc-codec-config failed' "$ffmpeg_log" ||
            fail 'HEVC Main inter translator failed unexpectedly'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=hevc-main-inter-prime-eos-transport-pass\n'
            printf 'ffmpeg_exit=%s\n' "$decode_status"
            printf 'prime_exports=%s\n' "$prime_exports"
            printf 'pixel_accuracy=not-yet-gated\n'
        } >"$summary"
        write_queue_evidence
        write_pass_verdict hevc-main-inter-prime-eos-transport-pass \
            ffmpegExit "$decode_status" primeExports "$prime_exports" \
            pixelAccuracyGated 0
        ;;
    hevc-main-pixel-accuracy)
        generate_hevc_normal
        run_hevc_pixel_accuracy
        grep -Eq 'lindex-eos-probe: context-summary .*submitted=4 .*eos=complete' "$ffmpeg_log" ||
            fail 'HEVC pixel gate did not observe four pictures and EOS'
        prime_exports=$(count_pattern 'lindex-eos-probe: prime-sync-pass' "$ffmpeg_log")
        [ "$prime_exports" -ge 4 ] ||
            fail "HEVC pixel gate PRIME exports were incomplete: $prime_exports"
        grep -Fq 'lindex-eos-probe: surface-release-pass' "$ffmpeg_log" ||
            fail 'HEVC pixel gate surface release lifecycle missing'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=hevc-main-inter-poc-pixel-accuracy-pass\n'
            printf 'ffmpeg_exit=%s\n' "$decode_status"
            printf 'prime_exports=%s\n' "$prime_exports"
            printf 'pixel_accuracy=poc-keyed-nv12-exact\n'
            printf 'pixel_set_sha256=%s\n' "$pixel_set_sha"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict hevc-main-inter-poc-pixel-accuracy-pass \
            ffmpegExit "$decode_status" primeExports "$prime_exports" \
            pixelAccuracyGated 1
        ;;
    vp9-profile0)
        generate_vp9
        run_decode validated-profile0-inter-v1 yes ''
        [ "$decode_status" -eq 0 ] || fail "VP9 Profile 0 decode exited $decode_status"
        grep -Eq 'frame=[[:space:]]*2([^0-9]|$)' "$ffmpeg_log" ||
            fail 'VP9 did not complete key plus inter frame'
        grep -Eq 'lindex-eos-probe: context-summary .*submitted=2 .*eos=complete' "$ffmpeg_log" ||
            fail 'VP9 probe did not observe exactly two submitted pictures and EOS'
        prime_exports=$(count_pattern 'lindex-eos-probe: prime-sync-pass' "$ffmpeg_log")
        [ "$prime_exports" -ge 2 ] || fail "VP9 PRIME exports were incomplete: $prime_exports"
        grep -Fq 'lindex-eos-probe: surface-release-pass' "$ffmpeg_log" || fail 'VP9 surface release lifecycle missing'
        grep -Fq 'lindex-eos-probe: eos-pass' "$ffmpeg_log" || fail 'VP9 EOS lifecycle missing'
        key_queues=$(count_pattern 'advc-codec: queue .*flags=0x2 ' "$broker_log")
        inter_queues=$(count_pattern 'advc-codec: queue .*flags=0x0 ' "$broker_log")
        eos_queues=$(count_pattern 'advc-codec: queue .*flags=0x1 ' "$broker_log")
        [ "$key_queues" -ge 1 ] || fail 'VP9 key-frame queue missing'
        [ "$inter_queues" -ge 1 ] || fail 'VP9 inter-frame queue missing'
        [ "$eos_queues" -ge 1 ] || fail 'VP9 terminal EOS queue missing'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=vp9-profile0-key-inter-prime-eos-pass\n'
            printf 'ffmpeg_exit=%s\n' "$decode_status"
            printf 'prime_exports=%s\n' "$prime_exports"
            printf 'key_queues=%s\n' "$key_queues"
            printf 'inter_queues=%s\n' "$inter_queues"
            printf 'eos_queues=%s\n' "$eos_queues"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict vp9-profile0-key-inter-prime-eos-pass \
            ffmpegExit "$decode_status" primeExports "$prime_exports" \
            keyQueues "$key_queues" interQueues "$inter_queues" \
            eosQueues "$eos_queues"
        ;;
    gstreamer-h264-vasurface-eos-teardown)
        generate_h264
        run_gstreamer_diagnostic
        software_class=$(sed -n 's/^classification=//p' \
            "$report_dir/gstreamer/software.summary")
        vaapi_class=$(sed -n 's/^classification=//p' \
            "$report_dir/gstreamer/vaapi.summary")
        [ "$software_class" = complete ] ||
            fail "GStreamer software baseline was $software_class"
        wait_for_broker_pattern \
            'advc-codec: destroy before-stop direction=1 discarded-codec-outputs=[0-9]+ destroy-drain=(enabled|disabled) session=0x[0-9a-fA-F]+$' \
            40 || fail 'decoder teardown never reached the broker before-stop boundary'
        destroy_before=$(grep -E \
            'advc-codec: destroy before-stop direction=1 discarded-codec-outputs=[0-9]+ destroy-drain=(enabled|disabled) session=0x[0-9a-fA-F]+$' \
            "$broker_log" | sed -n '$p')
        discarded_codec_outputs=$(printf '%s\n' "$destroy_before" |
            sed -n 's/.*discarded-codec-outputs=\([0-9][0-9]*\).*/\1/p')
        destroy_drain_state=$(printf '%s\n' "$destroy_before" |
            sed -n 's/.*destroy-drain=\([^ ]*\).*/\1/p')
        destroy_session=$(printf '%s\n' "$destroy_before" |
            sed -n 's/.*session=\([^ ]*\)$/\1/p')
        case "$discarded_codec_outputs" in
            ''|*[!0-9]*) fail 'invalid discarded-codec-outputs evidence' ;;
        esac
        case "$destroy_session" in
            0x[0-9a-fA-F]*) ;;
            *) fail 'invalid decoder teardown session evidence' ;;
        esac
        [ "$decode_status" -eq 0 ] ||
            fail "baseline GStreamer diagnostic exited $decode_status"
        [ "$vaapi_class" = complete ] ||
            fail "baseline did not complete GStreamer: $vaapi_class"
        [ "$destroy_drain_state" = disabled ] ||
            fail 'baseline unexpectedly enabled decoder destroy drain'
        [ "$discarded_codec_outputs" -eq 0 ] ||
            fail 'baseline unexpectedly discarded MediaCodec output'
        wait_for_broker_pattern \
            "advc-codec: destroy after-stop status=0 direction=1 session=$destroy_session$" \
            40 || fail 'baseline did not prove successful AMediaCodec_stop return for the same session'
        stop_returned=1
        classification=gstreamer-h264-vasurface-eos-teardown-pass
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=%s\n' "$classification"
            printf 'diagnostic_exit=%s\n' "$decode_status"
            printf 'software_classification=%s\n' "$software_class"
            printf 'vaapi_classification=%s\n' "$vaapi_class"
            printf 'destroy_drain=%s\n' "$destroy_drain_state"
            printf 'discarded_codec_outputs=%s\n' "$discarded_codec_outputs"
            printf 'stop_returned=%s\n' "$stop_returned"
            printf 'destroy_session=%s\n' "$destroy_session"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict "$classification" \
            diagnosticExit "$decode_status" \
            discardedCodecOutputs "$discarded_codec_outputs" \
            stopReturned "$stop_returned"
        ;;
    encode-h264-hevc-ffmpeg-gstreamer)
        run_encode_regression
        wait_for_broker_pattern 'advc-codec: created direction=2 mime=video/avc ' 40 ||
            fail 'broker did not create the hardware AVC encoder session'
        wait_for_broker_pattern 'advc-codec: created direction=2 mime=video/hevc ' 40 ||
            fail 'broker did not create the hardware HEVC encoder session'
        {
            printf 'case=%s\n' "$case_name"
            printf 'classification=ffmpeg-gstreamer-avc-hevc-bounded-encode-pass\n'
            printf 'ffmpeg_h264_frames=%s\n' "$ffmpeg_h264_frames"
            printf 'ffmpeg_hevc_frames=%s\n' "$ffmpeg_hevc_frames"
            printf 'gstreamer_h264_frames=%s\n' "$gstreamer_h264_frames"
            printf 'gstreamer_hevc_frames=%s\n' "$gstreamer_hevc_frames"
        } >"$summary"
        write_queue_evidence
        write_pass_verdict ffmpeg-gstreamer-avc-hevc-bounded-encode-pass \
            ffmpegH264Frames "$ffmpeg_h264_frames" \
            ffmpegHevcFrames "$ffmpeg_hevc_frames" \
            gstreamerH264Frames "$gstreamer_h264_frames" \
            gstreamerHevcFrames "$gstreamer_hevc_frames"
        ;;
esac

printf 'isolated decode case: PASS (%s)\n' "$case_name"
cat "$summary"
printf '%s\n' 'verdict_json:'
cat "$report_dir/verdict.json"
