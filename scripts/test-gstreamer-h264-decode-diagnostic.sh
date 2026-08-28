#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
diagnostic=$repo_dir/scripts/diagnose-gstreamer-h264-decode.sh
fixture=$repo_dir/tests/fixtures/gstreamer-h264-diagnostic/gst-launch-fixture.sh
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lindex-gst-h264-test.XXXXXX")
cleanup() {
    rm -rf -- "$work_dir"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'gstreamer h264 diagnostic test failed: %s\n' "$*" >&2
    exit 1
}

require_text() {
    grep -Fq -- "$1" "$2" || fail "missing '$1' in ${2#$repo_dir/}"
}

sh -n "$diagnostic" "$fixture" "$0"

if grep -Eq '(^|[;&|[:space:]])(pkill|killall)[[:space:]]|kill[[:space:]][^#\n]*gst-plugin-scanner' \
    "$diagnostic"; then
    fail 'diagnostic must not signal a plugin scanner'
fi

mkdir -p "$work_dir/state"
printf '\000' >"$work_dir/input sample.h264"

set +e
FIXTURE_STATE_DIR=$work_dir/state \
FIXTURE_HANG_VAAPI=1 \
GST_LAUNCH_BIN=$fixture \
GST_INSPECT_BIN=$fixture \
TIMEOUT_BIN=$(command -v timeout) \
    "$diagnostic" --timeout 1 --output-dir "$work_dir/report" \
    "$work_dir/input sample.h264" >"$work_dir/stdout" 2>"$work_dir/stderr"
diagnostic_status=$?
set -e

[ "$diagnostic_status" -eq 1 ] ||
    fail "expected diagnostic boundary status 1, got $diagnostic_status"

software_invocation=$work_dir/state/software.invocation
vaapi_invocation=$work_dir/state/vaapi.invocation
[ -f "$software_invocation" ] || fail 'software fixture was not invoked'
[ -f "$vaapi_invocation" ] || fail 'VA-API fixture was not invoked'

for invocation in "$software_invocation" "$vaapi_invocation"; do
    require_text 'GST_REGISTRY_FORK=no' "$invocation"
    require_text 'GST_DEBUG_NO_COLOR=1' "$invocation"
    require_text 'args=-m -v ' "$invocation"
    require_text 'filesrc' "$invocation"
    require_text "location=$work_dir/input sample.h264" "$invocation"
    require_text 'h264parse' "$invocation"
    require_text 'silent=false' "$invocation"
    require_text 'fakesink sync=false async=false enable-last-sample=false' \
        "$invocation"
done

require_text 'avdec_h264' "$software_invocation"
require_text 'name=decode_boundary_sw' "$software_invocation"
require_text 'ADVC_VAAPI_TRACE=' "$software_invocation"
require_text 'vaapih264dec' "$vaapi_invocation"
require_text 'video/x-raw(memory:VASurface)' "$vaapi_invocation"
require_text 'name=decode_boundary_va' "$vaapi_invocation"
require_text 'ADVC_VAAPI_TRACE=1' "$vaapi_invocation"

software_registry=$(sed -n 's/^GST_REGISTRY=//p' "$software_invocation")
vaapi_registry=$(sed -n 's/^GST_REGISTRY=//p' "$vaapi_invocation")
[ -n "$software_registry" ] || fail 'transient registry path is empty'
[ "$software_registry" = "$vaapi_registry" ] ||
    fail 'pipelines did not share the task-owned registry'
[ ! -e "$(dirname -- "$software_registry")" ] ||
    fail 'transient registry directory survived diagnostic exit'

require_text 'classification=complete' "$work_dir/report/software.summary"
require_text 'parser_sink_eos=seen' "$work_dir/report/software.summary"
require_text 'parser_src_eos=seen' "$work_dir/report/software.summary"
require_text 'finish_frame=seen' "$work_dir/report/software.summary"
require_text 'downstream_eos=seen' "$work_dir/report/software.summary"
require_text 'bus_eos=seen' "$work_dir/report/software.summary"

require_text 'classification=output-allocation-or-finish-frame-push-boundary' \
    "$work_dir/report/vaapi.summary"
require_text 'exit_code=124' "$work_dir/report/vaapi.summary"
require_text 'timed_out=yes' "$work_dir/report/vaapi.summary"
require_text 'parser_src_eos=seen' "$work_dir/report/vaapi.summary"
require_text 'finish_frame=seen' "$work_dir/report/vaapi.summary"
require_text 'downstream_buffer=missing' "$work_dir/report/vaapi.summary"
require_text 'downstream_eos=missing' "$work_dir/report/vaapi.summary"
require_text 'bus_eos=missing' "$work_dir/report/vaapi.summary"

require_text 'registry_scope=task-owned-transient' \
    "$work_dir/report/run-info.txt"
require_text 'registry_fork=no' "$work_dir/report/run-info.txt"
require_text 'Software: complete' "$work_dir/stdout"
require_text 'VA-API:   output-allocation-or-finish-frame-push-boundary' \
    "$work_dir/stdout"

require_text 'diagnose-gstreamer-h264-decode.sh' \
    "$repo_dir/docs/TROUBLESHOOTING.md"
require_text 'diagnose-gstreamer-h264-decode.sh' \
    "$repo_dir/docs/ko/TROUBLESHOOTING.md"

set +e
FIXTURE_STATE_DIR=$work_dir/state \
FIXTURE_HANG_AFTER_BUS_EOS=1 \
GST_LAUNCH_BIN=$fixture \
GST_INSPECT_BIN=$fixture \
TIMEOUT_BIN=$(command -v timeout) \
    "$diagnostic" --timeout 1 --output-dir "$work_dir/report-teardown" \
    "$work_dir/input sample.h264" >"$work_dir/stdout-teardown" \
    2>"$work_dir/stderr-teardown"
teardown_status=$?
set -e

[ "$teardown_status" -eq 1 ] ||
    fail "expected teardown boundary status 1, got $teardown_status"
require_text 'classification=post-bus-eos-process-timeout' \
    "$work_dir/report-teardown/vaapi.summary"
require_text 'timed_out=yes' "$work_dir/report-teardown/vaapi.summary"
require_text 'downstream_eos=seen' "$work_dir/report-teardown/vaapi.summary"
require_text 'bus_eos=seen' "$work_dir/report-teardown/vaapi.summary"
require_text 'VA-API:   post-bus-eos-process-timeout' \
    "$work_dir/stdout-teardown"

set +e
FIXTURE_STATE_DIR=$work_dir/state \
FIXTURE_FAIL_AFTER_BUS_EOS=1 \
GST_LAUNCH_BIN=$fixture \
GST_INSPECT_BIN=$fixture \
TIMEOUT_BIN=$(command -v timeout) \
    "$diagnostic" --timeout 3 --output-dir "$work_dir/report-process-error" \
    "$work_dir/input sample.h264" >"$work_dir/stdout-process-error" \
    2>"$work_dir/stderr-process-error"
process_error_status=$?
set -e

[ "$process_error_status" -eq 1 ] ||
    fail "expected post-EOS process error status 1, got $process_error_status"
require_text 'classification=post-bus-eos-process-error' \
    "$work_dir/report-process-error/vaapi.summary"
require_text 'exit_code=139' "$work_dir/report-process-error/vaapi.summary"
require_text 'timed_out=no' "$work_dir/report-process-error/vaapi.summary"
require_text 'bus_eos=seen' "$work_dir/report-process-error/vaapi.summary"
require_text 'VA-API:   post-bus-eos-process-error' \
    "$work_dir/stdout-process-error"

set +e
FIXTURE_STATE_DIR=$work_dir/state \
FIXTURE_MISSING_AVDEC=1 \
GST_LAUNCH_BIN=$fixture \
GST_INSPECT_BIN=$fixture \
TIMEOUT_BIN=$(command -v timeout) \
    "$diagnostic" --timeout 3 --output-dir "$work_dir/report-openh264" \
    "$work_dir/input sample.h264" >"$work_dir/stdout-openh264" \
    2>"$work_dir/stderr-openh264"
openh264_status=$?
set -e

[ "$openh264_status" -eq 1 ] ||
    fail "expected openh264dec fallback diagnostic status 1, got $openh264_status"
require_text 'software_decoder=openh264dec' \
    "$work_dir/report-openh264/run-info.txt"
require_text 'h264parse ! openh264dec ! identity' \
    "$work_dir/report-openh264/run-info.txt"
require_text 'openh264dec' "$work_dir/state/software.invocation"
require_text 'classification=complete' \
    "$work_dir/report-openh264/software.summary"

printf 'gstreamer h264 diagnostic fixture: PASS\n'
