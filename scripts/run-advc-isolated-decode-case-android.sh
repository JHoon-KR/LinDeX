#!/system/bin/sh
set -eu

# Android-host orchestrator for one live codec case.  It accepts only a
# /data/local/tmp staging directory, starts the staged broker on a unique
# socket, exposes only that unique directory to the chroot, and removes the
# exact PID/mount/path on every exit.  It never reads, replaces or signals the
# production broker.

usage() {
    cat <<'EOF'
Usage: run-advc-isolated-decode-case-android.sh \
  STAGE_DIR ROOTFS CASE

STAGE_DIR must match /data/local/tmp/lindex-codec-stage.* and contain:
  advc-broker
  advc-capability-probe
  advc_drv_video.so
  test-advc-isolated-decode-case.sh
  advc-ffmpeg-decode-eos-probe.so
  diagnose-gstreamer-h264-decode.sh
  advc-nv12-frame-hash (required only for hevc-main-pixel-accuracy)

ROOTFS is the existing Debian chroot (normally /data/local/debian).
Only one CASE from the guest gate is run per invocation, using a fresh broker.
EOF
}

fail() {
    echo "isolated Android decode runner failed: $*" >&2
    exit 1
}

[ "$#" -eq 3 ] || { usage >&2; exit 2; }
stage_dir=$1
rootfs=$2
case_name=$3

case "$stage_dir" in
    /data/local/tmp/lindex-codec-stage.*) ;;
    *) fail "unsafe staging directory refused: $stage_dir" ;;
esac
case "$rootfs" in
    /data/local/*) ;;
    *) fail "unexpected chroot path refused: $rootfs" ;;
esac
case "$case_name" in
    h264-baseline|hevc-narrow|hevc-main-inter|hevc-main-pixel-accuracy|vp9-profile0|\
gstreamer-h264-vasurface-eos-teardown|encode-h264-hevc-ffmpeg-gstreamer) ;;
    *) fail "unknown case: $case_name" ;;
esac

[ -d "$stage_dir" ] || fail "staging directory is missing: $stage_dir"
[ -d "$rootfs" ] || fail "chroot is missing: $rootfs"
for name in advc-broker advc-capability-probe advc_drv_video.so \
    test-advc-isolated-decode-case.sh advc-ffmpeg-decode-eos-probe.so \
    diagnose-gstreamer-h264-decode.sh; do
    [ -f "$stage_dir/$name" ] || fail "staged file is missing: $name"
done
[ "$case_name" != hevc-main-pixel-accuracy ] ||
    [ -x "$stage_dir/advc-nv12-frame-hash" ] ||
    fail 'staged ARM64 NV12 frame hash tool is missing or not executable'
[ -x "$stage_dir/advc-broker" ] || fail 'staged broker is not executable'
[ -x "$stage_dir/advc-capability-probe" ] || fail 'staged capability probe is not executable'

run_dir=$(mktemp -d /data/local/tmp/lindex-codec-live.XXXXXX) ||
    fail 'could not create transient Android run directory'
run_name=${run_dir##*/}
guest_mount_host=$rootfs/tmp/$run_name
guest_mount=/tmp/$run_name
socket_host=$run_dir/advc-case.sock
socket_guest=$guest_mount/advc-case.sock
broker_log_host=$run_dir/broker.log
broker_log_guest=$guest_mount/broker.log
report_guest=$guest_mount/report
broker_pid=
mounted=no
cleanup_done=no
cleanup_status=0

path_is_mounted() {
    awk -v wanted="$guest_mount_host" '$2 == wanted { found=1 } END { exit !found }' /proc/mounts
}

broker_identity_matches() {
    [ -n "$broker_pid" ] && [ -d "/proc/$broker_pid" ] || return 1
    expected=$(readlink -f "$run_dir/advc-broker" 2>/dev/null) || return 1
    actual=$(readlink -f "/proc/$broker_pid/exe" 2>/dev/null) || return 1
    [ "$actual" = "$expected" ] || return 1
    dd if="/proc/$broker_pid/cmdline" bs=4096 count=16 2>/dev/null |
        tr '\000' '\n' |
        grep -Fqx -- "$socket_host"
}

cleanup() {
    [ "$cleanup_done" = no ] || return "$cleanup_status"
    cleanup_done=yes
    cleanup_status=0

    if broker_identity_matches; then
        kill -TERM "$broker_pid" 2>/dev/null || true
        spin=0
        while [ "$spin" -lt 50 ] && [ -d "/proc/$broker_pid" ]; do
            sleep 0.02
            spin=$((spin + 1))
        done
        if broker_identity_matches; then
            kill -KILL "$broker_pid" 2>/dev/null || true
        fi
        wait "$broker_pid" 2>/dev/null || true
    elif [ -n "$broker_pid" ] && [ -d "/proc/$broker_pid" ]; then
        if grep -Eq '^State:[[:space:]]+Z' "/proc/$broker_pid/status" 2>/dev/null; then
            wait "$broker_pid" 2>/dev/null || true
        else
            echo "isolated Android decode cleanup failed: broker PID identity changed; preserving $run_dir" >&2
            cleanup_status=1
        fi
    fi
    if [ "$mounted" = yes ] && path_is_mounted; then
        spin=0
        while [ "$spin" -lt 3 ] && path_is_mounted; do
            umount "$guest_mount_host" 2>/dev/null || true
            spin=$((spin + 1))
            [ "$spin" -ge 3 ] || sleep 0.05
        done
        if path_is_mounted; then
            echo "isolated Android decode cleanup failed: exact bind mount remains at $guest_mount_host; preserving both transient directories" >&2
            cleanup_status=1
        else
            mounted=no
        fi
    fi
    if [ "$mounted" = no ]; then
        case "$guest_mount_host" in
            "$rootfs"/tmp/lindex-codec-live.*)
                if [ -e "$guest_mount_host" ] &&
                   ! rmdir "$guest_mount_host" 2>/dev/null; then
                    echo "isolated Android decode cleanup failed: guest mountpoint directory remains at $guest_mount_host" >&2
                    cleanup_status=1
                fi
                ;;
        esac
        if [ "$cleanup_status" -eq 0 ]; then
            case "$run_dir" in
                /data/local/tmp/lindex-codec-live.*)
                    if ! rm -rf "$run_dir"; then
                        echo "isolated Android decode cleanup failed: transient run directory remains at $run_dir" >&2
                        cleanup_status=1
                    fi
                    ;;
            esac
        fi
    fi
    return "$cleanup_status"
}

on_exit() {
    exit_status=$1
    trap - EXIT HUP INT TERM
    if ! cleanup && [ "$exit_status" -eq 0 ]; then
        exit_status=1
    fi
    exit "$exit_status"
}

on_signal() {
    signal_status=$1
    trap - EXIT HUP INT TERM
    cleanup || true
    exit "$signal_status"
}

trap 'on_exit $?' EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

mkdir -m 0700 "$guest_mount_host"
for name in advc-broker advc-capability-probe advc_drv_video.so \
    test-advc-isolated-decode-case.sh advc-ffmpeg-decode-eos-probe.so \
    diagnose-gstreamer-h264-decode.sh; do
    cp "$stage_dir/$name" "$run_dir/$name"
done
if [ "$case_name" = hevc-main-pixel-accuracy ]; then
    cp "$stage_dir/advc-nv12-frame-hash" "$run_dir/advc-nv12-frame-hash"
    chmod 0700 "$run_dir/advc-nv12-frame-hash"
fi
chmod 0700 "$run_dir/advc-broker" "$run_dir/advc-capability-probe" \
    "$run_dir/test-advc-isolated-decode-case.sh" \
    "$run_dir/diagnose-gstreamer-h264-decode.sh"
chmod 0600 "$run_dir/advc_drv_video.so" \
    "$run_dir/advc-ffmpeg-decode-eos-probe.so"

mount -o bind "$run_dir" "$guest_mount_host"
mounted=yes
path_is_mounted || fail 'exact transient bind mount was not registered'

ADVC_DEBUG=1 "$run_dir/advc-capability-probe" \
    >"$run_dir/capabilities.json" 2>"$run_dir/capabilities.log"

case "$case_name" in
    h264-*|gstreamer-h264-*) wanted_mime=video/avc ;;
    hevc-*) wanted_mime=video/hevc ;;
    vp9-*) wanted_mime=video/x-vnd.on2.vp9 ;;
    encode-h264-hevc-ffmpeg-gstreamer) wanted_mime=encode-matrix ;;
esac

# capabilities.json is a one-line object. Split only object boundaries, then
# select an actual decode entry which the shared classifier marked hardware.
if [ "$wanted_mime" = encode-matrix ]; then
    for mime in video/avc video/hevc; do
        codec_entry=$(sed 's/},{/}\
{/g' "$run_dir/capabilities.json" |
            grep -F "\"mime\":\"$mime\"" | grep -F '"direction":2' |
            grep -F '"acceleration":2' | sed -n '1p')
        [ -n "$codec_entry" ] || fail "no hardware encoder capability for $mime"
        codec_component=$(printf '%s\n' "$codec_entry" |
            sed -n 's/.*"codec":"\([^"]*\)".*/\1/p')
        case "$codec_component" in
            ''|c2.android.*|c2.google.*|c2.ffmpeg.*|OMX.google.*|OMX.ffmpeg.*)
                fail "software or empty encoder component refused: $codec_component"
                ;;
        esac
        printf 'capability-pass case=%s mime=%s component=%s acceleration=hardware\n' \
            "$case_name" "$mime" "$codec_component"
    done
    codec_component=hardware-encode-matrix
else
    codec_entry=$(sed 's/},{/}\
{/g' "$run_dir/capabilities.json" |
        grep -F "\"mime\":\"$wanted_mime\"" |
        grep -F '"direction":1' |
        grep -F '"acceleration":2' | sed -n '1p')
    [ -n "$codec_entry" ] || fail "no hardware decoder capability for $wanted_mime"
    codec_component=$(printf '%s\n' "$codec_entry" |
        sed -n 's/.*"codec":"\([^"]*\)".*/\1/p')
    [ -n "$codec_component" ] || fail 'hardware codec component name was not returned'
    case "$codec_component" in
        c2.android.*|c2.google.*|c2.ffmpeg.*|OMX.google.*|OMX.ffmpeg.*)
            fail "software codec component refused: $codec_component"
            ;;
    esac
    printf 'capability-pass case=%s mime=%s component=%s acceleration=hardware\n' \
        "$case_name" "$wanted_mime" "$codec_component"
fi

if [ "$wanted_mime" = encode-matrix ]; then
    ADVC_DEBUG=1 ADVC_DMABUF_BACKEND=vulkan \
    ADVC_VULKAN_ABSOLUTE_PRESENT=validated-v1 \
    ADVC_SURFACE_LOW_LATENCY=validated-v1 \
    ADVC_DECODE_PRIME_VALIDATION=validated-qcom-prime-repack-linear-fence-eos-v1 \
        "$run_dir/advc-broker" "$socket_host" \
        >"$broker_log_host" 2>&1 &
else
    ADVC_DEBUG=1 \
    ADVC_CODEC_NAME="$codec_component" \
    ADVC_CODEC_DECODER_DESTROY_DRAIN= \
    ADVC_DECODE_PRIME_VALIDATION=validated-qcom-prime-repack-linear-fence-eos-v1 \
        "$run_dir/advc-broker" "$socket_host" \
        >"$broker_log_host" 2>&1 &
fi
broker_pid=$!

spin=0
while [ "$spin" -lt 200 ] && [ ! -S "$socket_host" ]; do
    if [ ! -d "/proc/$broker_pid" ]; then
        cat "$broker_log_host" >&2 || true
        fail 'isolated broker exited before creating its socket'
    fi
    sleep 0.02
    spin=$((spin + 1))
done
[ -S "$socket_host" ] || fail 'isolated broker socket timed out'
broker_identity_matches || fail 'isolated broker PID identity check failed'

set +e
gstreamer_timeout=${ADVC_GSTREAMER_TIMEOUT:-10}
case "$gstreamer_timeout" in
    ''|*[!0-9]*) fail 'ADVC_GSTREAMER_TIMEOUT must be an integer from 1 to 60' ;;
esac
[ "$gstreamer_timeout" -ge 1 ] && [ "$gstreamer_timeout" -le 60 ] ||
    fail 'ADVC_GSTREAMER_TIMEOUT must be an integer from 1 to 60'
chroot "$rootfs" /usr/bin/env -i \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    LANG=C LC_ALL=C \
    ADVC_GSTREAMER_TIMEOUT="$gstreamer_timeout" \
    "$guest_mount/test-advc-isolated-decode-case.sh" \
    --case "$case_name" \
    --driver "$guest_mount/advc_drv_video.so" \
    --socket "$socket_guest" \
    --broker-log "$broker_log_guest" \
    --eos-probe "$guest_mount/advc-ffmpeg-decode-eos-probe.so" \
    --gstreamer-diagnostic "$guest_mount/diagnose-gstreamer-h264-decode.sh" \
    --pixel-hash-tool "$guest_mount/advc-nv12-frame-hash" \
    --report-dir "$report_guest"
guest_status=$?
set -e

echo '--- capability JSON ---'
cat "$run_dir/capabilities.json"
echo '--- guest summary ---'
cat "$run_dir/report/summary.txt" 2>/dev/null || true
echo '--- machine verdict JSON ---'
cat "$run_dir/report/verdict.json" 2>/dev/null || true
echo '--- relevant FFmpeg/driver evidence ---'
grep -E 'advc-vaapi: (slices=|pps-values|codec-config-hex|in-band-codec-config|hevc-codec-config failed)|lindex-eos-probe:|frame=' \
    "$run_dir/report/ffmpeg.log" 2>/dev/null || true
echo '--- broker queue evidence ---'
grep -E 'advc-codec: (created|started|queue |dequeue return|destroy before-stop|destroy after-stop)' \
    "$broker_log_host" 2>/dev/null || true
if [ "$guest_status" -ne 0 ]; then
    echo '--- failed guest FFmpeg tail ---'
    tail -n 200 "$run_dir/report/ffmpeg.log" 2>/dev/null || true
    echo '--- failed guest GStreamer diagnostic ---'
    cat "$run_dir/report/gstreamer.stdout" 2>/dev/null || true
    echo '--- failed guest GStreamer summaries ---'
    cat "$run_dir/report/gstreamer/software.summary" 2>/dev/null || true
    cat "$run_dir/report/gstreamer/vaapi.summary" 2>/dev/null || true
    echo '--- failed guest GStreamer EOS boundary evidence ---'
    grep -Ei 'eos|drain|finish[_ -]?frame|pop frame|decode_boundary_va|fakesink' \
        "$run_dir/report/gstreamer/vaapi.log" 2>/dev/null | tail -n 240 || true
    echo '--- failed guest GStreamer bin EOS decision ---'
    grep -E "handling child .* message of type eos|sink '.*' (posted EOS|did not post EOS yet)|all sinks posted EOS|Not forwarding EOS|posted_playing|state change" \
        "$run_dir/report/gstreamer/vaapi.log" 2>/dev/null | tail -n 180 || true
fi

[ "$guest_status" -eq 0 ] || fail "guest case exited $guest_status"
cleanup || fail 'exact transient broker, bind mount or run-directory cleanup failed'
printf 'isolated Android decode runner: PASS (%s, %s)\n' \
    "$case_name" "$codec_component"
