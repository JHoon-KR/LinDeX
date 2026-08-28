#include "advc_vaapi_encode.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void add_codec(struct advc_capability_set *caps, const char *mime,
                      uint8_t acceleration, uint32_t width,
                      uint32_t height) {
    struct advc_codec_capability *codec = &caps->codecs[caps->count++];
    memset(codec, 0, sizeof(*codec));
    strcpy(codec->mime, mime);
    if (strcmp(mime, "video/avc") == 0)
        strcpy(codec->codec_name, "c2.qti.avc.encoder");
    else if (strcmp(mime, "video/hevc") == 0)
        strcpy(codec->codec_name, "c2.qti.hevc.encoder");
    codec->direction = ADVC_DIRECTION_ENCODE;
    codec->acceleration = acceleration;
    codec->max_width = width;
    codec->max_height = height;
}

static void test_policy(void) {
    struct advc_capability_set caps;
    struct advc_vaapi_encode_policy policy;
    VAConfigAttrib attrs[2];
    struct advc_vaapi_encode_config config;
    uint32_t value;

    memset(&caps, 0, sizeof(caps));
    caps.transport_features = ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
                              ADVC_FEATURE_NATIVE_FENCE |
                              ADVC_FEATURE_DMABUF_VULKAN;
    add_codec(&caps, "video/avc", ADVC_ACCELERATION_HARDWARE, 4096, 2160);
    add_codec(&caps, "video/hevc", ADVC_ACCELERATION_HARDWARE, 8192, 4320);
    add_codec(&caps, "video/av01", ADVC_ACCELERATION_SOFTWARE, 1920, 1080);
    assert(advc_vaapi_encode_policy_from_capabilities(&caps, &policy) == 0);
    assert(policy.codecs ==
           (ADVC_VAAPI_ENCODE_H264 | ADVC_VAAPI_ENCODE_HEVC));
    assert(policy.rate_control == VA_RC_VBR);
    assert(policy.h264_max_width == 4096 && policy.hevc_max_width == 8192);
    assert(advc_vaapi_encode_profile_supported(
        &policy, VAProfileH264ConstrainedBaseline));
    assert(!advc_vaapi_encode_profile_supported(&policy,
                                                VAProfileH264Main));
    assert(!advc_vaapi_encode_profile_supported(&policy,
                                                VAProfileH264High));
    assert(advc_vaapi_encode_profile_supported(&policy, VAProfileHEVCMain));
    assert(!advc_vaapi_encode_profile_supported(&policy,
                                                VAProfileHEVCMain10));
    assert(!advc_vaapi_encode_profile_supported(&policy, VAProfileVP9Profile0));
    assert(!advc_vaapi_encode_profile_supported(&policy,
                                                VAProfileAV1Profile0));

    assert(advc_vaapi_encode_get_attribute(
               &policy, VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice,
               VAConfigAttribRateControl, &value) == 0);
    assert(value == VA_RC_VBR);
    assert(advc_vaapi_encode_get_attribute(
               &policy, VAProfileH264ConstrainedBaseline,
               VAEntrypointEncSliceLP,
               VAConfigAttribRateControl, &value) < 0);

    attrs[0].type = VAConfigAttribRTFormat;
    attrs[0].value = VA_RT_FORMAT_YUV420;
    attrs[1].type = VAConfigAttribRateControl;
    attrs[1].value = VA_RC_VBR;
    assert(advc_vaapi_encode_config_init(
               &policy, VAProfileH264ConstrainedBaseline,
               VAEntrypointEncSlice, attrs, 2,
               &config) == 0);
    assert(config.codec == ADVC_VAAPI_ENCODE_CODEC_H264);
    assert(advc_vaapi_encode_config_init(
               &policy, VAProfileHEVCMain, VAEntrypointEncSlice, NULL, 0,
               &config) == 0);
    assert(config.codec == ADVC_VAAPI_ENCODE_CODEC_HEVC &&
           config.rate_control == VA_RC_VBR);
    attrs[1].value = VA_RC_CBR;
    assert(advc_vaapi_encode_config_init(
               &policy, VAProfileH264ConstrainedBaseline,
               VAEntrypointEncSlice, attrs, 2,
               &config) < 0);
    attrs[1].value = VA_RC_VBR | VA_RC_CBR;
    assert(advc_vaapi_encode_config_init(
               &policy, VAProfileH264ConstrainedBaseline,
               VAEntrypointEncSlice, attrs, 2,
               &config) < 0);

    strcpy(caps.codecs[0].codec_name, "c2.mtk.avc.encoder");
    assert(advc_vaapi_encode_policy_from_capabilities(&caps, &policy) == 0);
    assert(policy.codecs ==
           (ADVC_VAAPI_ENCODE_H264 | ADVC_VAAPI_ENCODE_HEVC));
    caps.codecs[0].acceleration = ADVC_ACCELERATION_SOFTWARE;
    assert(advc_vaapi_encode_policy_from_capabilities(&caps, &policy) == 0);
    assert(policy.codecs == ADVC_VAAPI_ENCODE_HEVC);
    caps.codecs[0].acceleration = ADVC_ACCELERATION_HARDWARE;

    caps.transport_features &= ~ADVC_FEATURE_NATIVE_FENCE;
    assert(advc_vaapi_encode_policy_from_capabilities(&caps, &policy) == 0);
    assert(policy.codecs == 0 && !policy.prime_input_ready);

    memset(&caps, 0, sizeof(caps));
    caps.transport_features = ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
                              ADVC_FEATURE_NATIVE_FENCE |
                              ADVC_FEATURE_DMABUF_EGL;
    add_codec(&caps, "video/avc", ADVC_ACCELERATION_HARDWARE, 0, 0);
    assert(advc_vaapi_encode_policy_from_capabilities(&caps, &policy) == 0);
    assert(policy.codecs == ADVC_VAAPI_ENCODE_H264);
    assert(policy.h264_max_width == 0 && policy.h264_max_height == 0);
}

static void test_h264_buffers(void) {
    struct advc_vaapi_encode_frame_params frame;
    VAEncSequenceParameterBufferH264 seq;
    VAEncPictureParameterBufferH264 pic;
    VAEncSliceParameterBufferH264 slice;
    _Alignas(VAEncMiscParameterBuffer)
        unsigned char fps_storage[sizeof(VAEncMiscParameterBuffer) +
                                  sizeof(VAEncMiscParameterFrameRate)];
    VAEncMiscParameterBuffer *fps = (VAEncMiscParameterBuffer *)fps_storage;
    VAEncMiscParameterFrameRate *fps_value =
        (VAEncMiscParameterFrameRate *)fps->data;

    advc_vaapi_encode_frame_init(&frame, ADVC_VAAPI_ENCODE_CODEC_H264,
                                 1920, 1080, 6000000, 60000);
    memset(&seq, 0, sizeof(seq));
    seq.picture_width_in_mbs = 120;
    seq.picture_height_in_mbs = 68;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.bits_per_second = 8000000;
    seq.intra_idr_period = 120;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSequenceParameterBufferType, &seq, sizeof(seq),
               1) == 0);

    memset(&pic, 0, sizeof(pic));
    pic.coded_buf = 19;
    pic.pic_fields.bits.idr_pic_flag = 1;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncPictureParameterBufferType, &pic, sizeof(pic),
               1) == 0);
    memset(&slice, 0, sizeof(slice));
    slice.num_macroblocks = 120 * 68;
    slice.slice_type = 2;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSliceParameterBufferType, &slice, sizeof(slice),
               1) == 0);

    memset(fps_storage, 0, sizeof(fps_storage));
    fps->type = VAEncMiscParameterTypeFrameRate;
    fps_value->framerate = (1001u << 16) | 60000u;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncMiscParameterBufferType, fps,
               sizeof(fps_storage), 1) == 0);
    assert(frame.framerate_milli == 59940);
    assert(frame.bitrate == 8000000 && frame.gop_frames == 120);
    assert(frame.force_idr && frame.coded_buffer == 19);
    assert(advc_vaapi_encode_frame_validate(&frame) == 0);

    advc_vaapi_encode_frame_begin(&frame);
    assert(frame.sequence_seen && !frame.picture_seen && !frame.slice_seen);
    slice.slice_type = 1;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSliceParameterBufferType, &slice, sizeof(slice),
               1) < 0);
}

static void test_hevc_buffers(void) {
    struct advc_vaapi_encode_frame_params frame;
    VAEncSequenceParameterBufferHEVC seq;
    VAEncPictureParameterBufferHEVC pic;
    VAEncSliceParameterBufferHEVC slice;

    advc_vaapi_encode_frame_init(&frame, ADVC_VAAPI_ENCODE_CODEC_HEVC,
                                 3840, 2160, 20000000, 30000);
    memset(&seq, 0, sizeof(seq));
    seq.general_profile_idc = 1;
    seq.pic_width_in_luma_samples = 3840;
    seq.pic_height_in_luma_samples = 2160;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.bits_per_second = 18000000;
    seq.intra_period = 60;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSequenceParameterBufferType, &seq, sizeof(seq),
               1) == 0);
    memset(&pic, 0, sizeof(pic));
    pic.coded_buf = 71;
    pic.pic_fields.bits.idr_pic_flag = 1;
    pic.pic_fields.bits.coding_type = 1;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncPictureParameterBufferType, &pic, sizeof(pic),
               1) == 0);
    memset(&slice, 0, sizeof(slice));
    slice.slice_type = 2;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSliceParameterBufferType, &slice, sizeof(slice),
               1) == 0);
    assert(advc_vaapi_encode_frame_validate(&frame) == 0);

    slice.slice_type = 0;
    assert(advc_vaapi_encode_frame_consume(
               &frame, VAEncSliceParameterBufferType, &slice, sizeof(slice),
               1) < 0);
}

int main(void) {
    test_policy();
    test_h264_buffers();
    test_hevc_buffers();
    return 0;
}
