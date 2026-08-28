#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: diagnose-gstreamer-h264-decode.sh [options] INPUT.h264

Run bounded software and VA-API H.264 pipelines against the same input.

Options:
  --timeout SECONDS   Per-pipeline timeout (default: 30, range: 1-300)
  --output-dir DIR    New or empty report directory
  -h, --help          Show this help

Run this opt-in developer diagnostic inside an already validated LinDeX
session. It does not enable ADVC capabilities or alter the session registry.
EOF
}

fail() {
    printf 'gstreamer h264 diagnostic: %s\n' "$*" >&2
    exit 2
}

have_log_pattern() {
    log_path=$1
    log_pattern=$2
    LC_ALL=C grep -Eiq -- "$log_pattern" "$log_path"
}

pattern_state() {
    if have_log_pattern "$1" "$2"; then
        printf '%s\n' seen
    else
        printf '%s\n' missing
    fi
}

classify_log() {
    classify_path=$1
    classify_pipeline=$2
    classify_status=$3

    parser_sink_eos=$(pattern_state "$classify_path" \
        '(<h264parse[^>]*:sink>|h264parse[^[:space:]]*:sink).*eos|eos.*(<h264parse[^>]*:sink>|h264parse[^[:space:]]*:sink)')
    parser_src_eos=$(pattern_state "$classify_path" \
        '(<h264parse[^>]*:src>|h264parse[^[:space:]]*:src).*eos|eos.*(<h264parse[^>]*:src>|h264parse[^[:space:]]*:src)')
    decoder_drain=$(pattern_state "$classify_path" \
        'gst_video_decoder_drain_out|gst_vaapidecode_(finish|drain)|decoder[^[:cntrl:]]*(finish|drain)[^[:cntrl:]]*(avdec_h264|openh264dec|vaapih264dec)')
    finish_frame=$(pattern_state "$classify_path" \
        'gst_video_decoder_finish_frame|finish[_ -]?frame')
    internal_frame_pop=$(pattern_state "$classify_path" \
        'pop frame [0-9]+ \(surface 0x[0-9a-f]+\)')
    downstream_buffer=$(pattern_state "$classify_path" \
        'decode_boundary_(sw|va)[^[:cntrl:]]*(chain|buffer)|(chain|buffer)[^[:cntrl:]]*decode_boundary_(sw|va)')
    downstream_eos=$(pattern_state "$classify_path" \
        '(<decode_boundary_(sw|va)[^>]*:(sink|src)>|<fakesink[^>]*:sink>|decode_boundary_(sw|va)[^[:space:]]*:(sink|src)|fakesink[^[:space:]]*:sink).*eos|eos.*(<decode_boundary_(sw|va)[^>]*:(sink|src)>|<fakesink[^>]*:sink>|decode_boundary_(sw|va)[^[:space:]]*:(sink|src)|fakesink[^[:space:]]*:sink)')
    bus_eos=$(pattern_state "$classify_path" \
        'got eos from element|gstmessageeos|message[^[:cntrl:]]*\(eos\)')
    pipeline_error=$(pattern_state "$classify_path" \
        'error: pipeline|erroneous pipeline|no element[[:space:]]+"?(h264parse|avdec_h264|openh264dec|vaapih264dec)|not-negotiated|could not link')

    case "$classify_status" in
        124|137) timed_out=yes ;;
        *) timed_out=no ;;
    esac

    if [ "$pipeline_error" = seen ]; then
        classification=setup-or-pipeline-error
    elif [ "$bus_eos" = seen ] && [ "$timed_out" = yes ]; then
        classification=post-bus-eos-process-timeout
    elif [ "$bus_eos" = seen ] && [ "$classify_status" -eq 0 ]; then
        classification=complete
    elif [ "$bus_eos" = seen ]; then
        classification=post-bus-eos-process-error
    elif [ "$parser_sink_eos" = missing ] && [ "$parser_src_eos" = missing ]; then
        classification=upstream-or-parser-eos-not-observed
    elif [ "$parser_sink_eos" = seen ] && [ "$parser_src_eos" = missing ]; then
        classification=parser-eos-forward-boundary
    elif [ "$downstream_eos" = seen ]; then
        classification=basesink-to-bus-eos-boundary
    elif [ "$finish_frame" = seen ] && [ "$downstream_buffer" = missing ]; then
        classification=output-allocation-or-finish-frame-push-boundary
    elif [ "$downstream_buffer" = seen ]; then
        classification=decoder-to-downstream-eos-boundary
    elif [ "$decoder_drain" = seen ]; then
        classification=decoder-finish-or-dpb-drain-boundary
    else
        classification=after-parser-eos-undetermined
    fi

    printf 'pipeline=%s\n' "$classify_pipeline"
    printf 'exit_code=%s\n' "$classify_status"
    printf 'timed_out=%s\n' "$timed_out"
    printf 'parser_sink_eos=%s\n' "$parser_sink_eos"
    printf 'parser_src_eos=%s\n' "$parser_src_eos"
    printf 'decoder_drain=%s\n' "$decoder_drain"
    printf 'finish_frame=%s\n' "$finish_frame"
    printf 'internal_frame_pop=%s\n' "$internal_frame_pop"
    printf 'downstream_buffer=%s\n' "$downstream_buffer"
    printf 'downstream_eos=%s\n' "$downstream_eos"
    printf 'bus_eos=%s\n' "$bus_eos"
    printf 'pipeline_error=%s\n' "$pipeline_error"
    printf 'classification=%s\n' "$classification"
}

timeout_seconds=30
output_dir=
input_path=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --timeout)
            [ "$#" -ge 2 ] || fail '--timeout requires a value'
            timeout_seconds=$2
            shift 2
            ;;
        --timeout=*)
            timeout_seconds=${1#*=}
            shift
            ;;
        --output-dir)
            [ "$#" -ge 2 ] || fail '--output-dir requires a value'
            output_dir=$2
            shift 2
            ;;
        --output-dir=*)
            output_dir=${1#*=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            fail "unknown option: $1"
            ;;
        *)
            [ -z "$input_path" ] || fail 'only one input file is accepted'
            input_path=$1
            shift
            ;;
    esac
done

while [ "$#" -gt 0 ]; do
    [ -z "$input_path" ] || fail 'only one input file is accepted'
    input_path=$1
    shift
done

case "$timeout_seconds" in
    ''|*[!0-9]*) fail '--timeout must be an integer from 1 to 300' ;;
esac
[ "$timeout_seconds" -ge 1 ] && [ "$timeout_seconds" -le 300 ] ||
    fail '--timeout must be an integer from 1 to 300'
[ -n "$input_path" ] || fail 'an H.264 input file is required'
[ -f "$input_path" ] && [ -r "$input_path" ] ||
    fail "input is not a readable file: $input_path"

input_dir=$(CDPATH= cd -- "$(dirname -- "$input_path")" && pwd)
input_path=$input_dir/$(basename -- "$input_path")

if [ -z "$output_dir" ]; then
    output_dir=./lindex-gst-h264-diagnostic-$(date +%Y%m%d-%H%M%S)-$$
fi
mkdir -p -- "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)

for report_name in software.log software.summary vaapi.log vaapi.summary \
    summary.txt run-info.txt; do
    [ ! -e "$output_dir/$report_name" ] ||
        fail "report directory is not empty: $output_dir"
done

gst_launch_bin=${GST_LAUNCH_BIN:-gst-launch-1.0}
gst_inspect_bin=${GST_INSPECT_BIN:-gst-inspect-1.0}
timeout_bin=${TIMEOUT_BIN:-timeout}
command -v "$gst_launch_bin" >/dev/null 2>&1 ||
    fail "gst-launch command not found: $gst_launch_bin"
command -v "$gst_inspect_bin" >/dev/null 2>&1 ||
    fail "gst-inspect command not found: $gst_inspect_bin"
command -v "$timeout_bin" >/dev/null 2>&1 ||
    fail "timeout command not found: $timeout_bin"

umask 077
task_dir=$(mktemp -d "${TMPDIR:-/tmp}/lindex-gst-h264.XXXXXX") ||
    fail 'could not create transient task directory'
cleanup() {
    rm -rf -- "$task_dir"
}
trap cleanup EXIT HUP INT TERM
registry_path=$task_dir/registry.bin

if env GST_REGISTRY="$registry_path" GST_REGISTRY_FORK=no \
       "$gst_inspect_bin" avdec_h264 >/dev/null 2>&1; then
    software_decoder=avdec_h264
elif env GST_REGISTRY="$registry_path" GST_REGISTRY_FORK=no \
         "$gst_inspect_bin" openh264dec >/dev/null 2>&1; then
    software_decoder=openh264dec
else
    fail 'neither avdec_h264 nor openh264dec is available for the software baseline'
fi

gst_debug='2,bin:7,GST_STATES:6,GST_EVENT:6,GST_PADS:6,vaapi*:7,videodecoder:7,h264parse:6,basesink:7,fakesink:7,bufferpool:6,identity:7'

run_pipeline() {
    run_kind=$1
    run_log=$2
    run_stdin=$task_dir/$run_kind.stdin
    mkfifo -- "$run_stdin"

    set +e
    case "$run_kind" in
        software)
            env \
                GST_REGISTRY="$registry_path" \
                GST_REGISTRY_FORK=no \
                GST_DEBUG_NO_COLOR=1 \
                GST_DEBUG="$gst_debug" \
                ADVC_VAAPI_TRACE= \
                "$timeout_bin" --signal=TERM --kill-after=2s \
                "${timeout_seconds}s" \
                "$gst_launch_bin" -m -v \
                filesrc "location=$input_path" ! \
                h264parse ! "$software_decoder" ! \
                identity name=decode_boundary_sw silent=false ! \
                fakesink sync=false async=false enable-last-sample=false \
                9<>"$run_stdin" <&9 >"$run_log" 2>&1
            ;;
        vaapi)
            env \
                GST_REGISTRY="$registry_path" \
                GST_REGISTRY_FORK=no \
                GST_DEBUG_NO_COLOR=1 \
                GST_DEBUG="$gst_debug" \
                ADVC_VAAPI_TRACE=1 \
                "$timeout_bin" --signal=TERM --kill-after=2s \
                "${timeout_seconds}s" \
                "$gst_launch_bin" -m -v \
                filesrc "location=$input_path" ! \
                h264parse ! vaapih264dec ! \
                'video/x-raw(memory:VASurface)' ! \
                identity name=decode_boundary_va silent=false ! \
                fakesink sync=false async=false enable-last-sample=false \
                9<>"$run_stdin" <&9 >"$run_log" 2>&1
            ;;
        *)
            set -e
            fail "unknown pipeline kind: $run_kind"
            ;;
    esac
    run_status=$?
    set -e
    rm -f -- "$run_stdin"
    return "$run_status"
}

{
    printf 'input=%s\n' "$input_path"
    printf 'timeout_seconds=%s\n' "$timeout_seconds"
    printf 'registry_scope=task-owned-transient\n'
    printf 'registry_fork=no\n'
    printf 'software_decoder=%s\n' "$software_decoder"
    printf 'software_pipeline=filesrc ! h264parse ! %s ! identity ! fakesink\n' \
        "$software_decoder"
    printf 'vaapi_pipeline=filesrc ! h264parse ! vaapih264dec ! video/x-raw(memory:VASurface) ! identity ! fakesink\n'
} >"$output_dir/run-info.txt"

printf 'Running software H.264 baseline (timeout %ss)...\n' "$timeout_seconds"
if run_pipeline software "$output_dir/software.log"; then
    software_status=0
else
    software_status=$?
fi
classify_log "$output_dir/software.log" software "$software_status" \
    >"$output_dir/software.summary"

printf 'Running VA-API VASurface H.264 pipeline (timeout %ss)...\n' \
    "$timeout_seconds"
if run_pipeline vaapi "$output_dir/vaapi.log"; then
    vaapi_status=0
else
    vaapi_status=$?
fi
classify_log "$output_dir/vaapi.log" vaapi "$vaapi_status" \
    >"$output_dir/vaapi.summary"

{
    cat "$output_dir/software.summary"
    printf '\n'
    cat "$output_dir/vaapi.summary"
} >"$output_dir/summary.txt"

software_class=$(sed -n 's/^classification=//p' \
    "$output_dir/software.summary")
vaapi_class=$(sed -n 's/^classification=//p' "$output_dir/vaapi.summary")

printf 'Software: %s\n' "$software_class"
printf 'VA-API:   %s\n' "$vaapi_class"
printf 'Report:   %s\n' "$output_dir"

if [ "$software_class" = complete ] && [ "$vaapi_class" = complete ]; then
    exit 0
fi
exit 1
