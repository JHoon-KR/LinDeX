#!/bin/sh
set -eu

: "${FIXTURE_STATE_DIR:?FIXTURE_STATE_DIR is required}"

if [ "${FIXTURE_MISSING_AVDEC-0}" = 1 ] && [ "$#" -eq 1 ] &&
   [ "$1" = avdec_h264 ]; then
    exit 1
fi

case " $* " in
    *' avdec_h264 '*)
        fixture_kind=software
        boundary_name=decode_boundary_sw
        decoder_name=avdec_h264-0
        ;;
    *' openh264dec '*)
        fixture_kind=software
        boundary_name=decode_boundary_sw
        decoder_name=openh264dec0
        ;;
    *' vaapih264dec '*)
        fixture_kind=vaapi
        boundary_name=decode_boundary_va
        decoder_name=vaapih264dec0
        ;;
    *)
        printf 'ERROR: erroneous pipeline: expected decoder is absent\n' >&2
        exit 1
        ;;
esac

{
    printf 'GST_REGISTRY=%s\n' "${GST_REGISTRY-}"
    printf 'GST_REGISTRY_FORK=%s\n' "${GST_REGISTRY_FORK-}"
    printf 'GST_DEBUG_NO_COLOR=%s\n' "${GST_DEBUG_NO_COLOR-}"
    printf 'ADVC_VAAPI_TRACE=%s\n' "${ADVC_VAAPI_TRACE-}"
    printf 'args=%s\n' "$*"
} >"$FIXTURE_STATE_DIR/$fixture_kind.invocation"

printf 'GST_EVENT gst_pad_send_event_unchecked:<h264parse0:sink> event type eos\n'
printf 'GST_EVENT gst_pad_push_event_unchecked:<h264parse0:src> event type eos\n'
printf 'videodecoder gst_video_decoder_drain_out:<%s> draining at EOS\n' \
    "$decoder_name"
printf 'vaapi pop frame 1 (surface 0x00000001)\n'
printf 'videodecoder gst_video_decoder_finish_frame:<%s> finish frame 1\n' \
    "$decoder_name"

if [ "$fixture_kind" = software ]; then
    printf 'identity <%s> chain buffer 0x1\n' "$boundary_name"
    printf 'GST_EVENT <%s:src> event type eos\n' "$boundary_name"
    printf 'GST_EVENT <fakesink0:sink> event type eos\n'
    printf 'Got message #1 from element "pipeline0" (eos): GstMessageEOS;\n'
    printf 'Got EOS from element "pipeline0".\n'
elif [ "${FIXTURE_HANG_AFTER_BUS_EOS-0}" = 1 ]; then
    printf 'identity <%s> chain buffer 0x1\n' "$boundary_name"
    printf 'GST_EVENT <%s:src> event type eos\n' "$boundary_name"
    printf 'GST_EVENT <fakesink0:sink> event type eos\n'
    printf 'Got message #1 from element "pipeline0" (eos): GstMessageEOS;\n'
    printf 'Got EOS from element "pipeline0".\n'
    sleep 10
elif [ "${FIXTURE_FAIL_AFTER_BUS_EOS-0}" = 1 ]; then
    printf 'identity <%s> chain buffer 0x1\n' "$boundary_name"
    printf 'GST_EVENT <%s:src> event type eos\n' "$boundary_name"
    printf 'GST_EVENT <fakesink0:sink> event type eos\n'
    printf 'Got message #1 from element "pipeline0" (eos): GstMessageEOS;\n'
    printf 'Got EOS from element "pipeline0".\n'
    exit 139
elif [ "${FIXTURE_HANG_VAAPI-0}" = 1 ]; then
    sleep 10
fi
