#include "advc_h264_annexb.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct advc_h264_parameter_input progressive_1080p(void) {
    struct advc_h264_parameter_input input;
    memset(&input, 0, sizeof(input));
    input.profile_idc = 77;
    input.visible_width = 1920;
    input.visible_height = 1080;
    input.picture_width_in_mbs_minus1 = 119;
    input.picture_height_in_mbs_minus1 = 67;
    input.chroma_format_idc = 1;
    input.frame_mbs_only_flag = 1;
    input.direct_8x8_inference_flag = 1;
    input.pic_order_cnt_type = 0;
    input.log2_max_pic_order_cnt_lsb_minus4 = 2;
    input.num_ref_frames = 4;
    input.entropy_coding_mode_flag = 1;
    input.deblocking_filter_control_present_flag = 1;
    input.num_ref_idx_l0_default_active_minus1 = 1;
    input.num_ref_idx_l1_default_active_minus1 = 0;
    return input;
}

static void test_slice_identity_and_au(void) {
    static const uint8_t idr[] = {0x65, 0xb8, 0x00};
    static const uint8_t p[] = {0x00, 0x00, 0x01, 0x41, 0xe2, 0x00};
    static const uint8_t expected_idr_au[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, 0xb8, 0x00,
    };
    struct advc_h264_slice_input slice;
    uint8_t *au = NULL;
    size_t au_size = 0;
    uint32_t first_mb;
    uint32_t slice_type;
    uint32_t pps_id;
    int is_idr;

    assert(advc_h264_parse_slice_identity(idr, sizeof(idr), &first_mb,
                                          &slice_type, &pps_id,
                                          &is_idr) == 0);
    assert(first_mb == 0 && slice_type == 2 && pps_id == 0 && is_idr == 1);
    slice.data = idr;
    slice.size = sizeof(idr);
    slice.expected_first_mb_in_slice = 0;
    slice.expected_slice_type = 2;
    assert(advc_h264_build_access_unit(&slice, 1, &au, &au_size, &pps_id,
                                       &is_idr) == 0);
    assert(au_size == sizeof(expected_idr_au));
    assert(memcmp(au, expected_idr_au, sizeof(expected_idr_au)) == 0);
    free(au);

    /* first_mb=0, slice_type=0, pps_id=0; Annex-B prefix is preserved once. */
    assert(advc_h264_parse_slice_identity(p, sizeof(p), &first_mb,
                                          &slice_type, &pps_id,
                                          &is_idr) == 0);
    assert(first_mb == 0 && slice_type == 0 && pps_id == 0 && is_idr == 0);
}

static void test_four_slice_p_access_unit(void) {
    static const uint8_t slice_data[4][2] = {
        {0x41, 0xe0}, /* first_mb_in_slice = 0 */
        {0x41, 0x58}, /* first_mb_in_slice = 1 */
        {0x41, 0x78}, /* first_mb_in_slice = 2 */
        {0x41, 0x26}, /* first_mb_in_slice = 3 */
    };
    struct advc_h264_slice_input slices[4];
    uint8_t *au = NULL;
    size_t au_size = 0;
    uint32_t pps_id = UINT32_MAX;
    int is_idr = 1;
    size_t i;

    memset(slices, 0, sizeof(slices));
    for (i = 0; i < 4; ++i) {
        slices[i].data = slice_data[i];
        slices[i].size = sizeof(slice_data[i]);
        slices[i].expected_first_mb_in_slice = (uint32_t)i;
        slices[i].expected_slice_type = 0;
    }
    assert(advc_h264_build_access_unit(slices, 4, &au, &au_size,
                                       &pps_id, &is_idr) == 0);
    assert(au_size == 4u * (4u + sizeof(slice_data[0])));
    assert(pps_id == 0 && is_idr == 0);
    for (i = 0; i < 4; ++i) {
        size_t offset = i * (4u + sizeof(slice_data[0]));
        assert(au[offset] == 0 && au[offset + 1] == 0 &&
               au[offset + 2] == 0 && au[offset + 3] == 1);
        assert(memcmp(au + offset + 4, slice_data[i],
                      sizeof(slice_data[i])) == 0);
    }
    free(au);
}

static void test_deterministic_sps_pps(void) {
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x00, 0x33,
        0xec, 0xa0, 0x3c, 0x01, 0x13, 0xf2, 0xa0, 0x00,
        0x00, 0x00, 0x01, 0x68, 0xea, 0x8f, 0x20,
    };
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_codec_config config;

    assert(advc_h264_build_codec_config(&input, 0, &config) == 0);
    assert(config.pps_id == 0);
    assert(config.size == sizeof(expected));
    if (memcmp(config.data, expected, sizeof(expected)) != 0) {
        size_t i;
        fputs("actual config:", stderr);
        for (i = 0; i < config.size; ++i)
            fprintf(stderr, " %02x", config.data[i]);
        fputc('\n', stderr);
    }
    assert(memcmp(config.data, expected, sizeof(expected)) == 0);
}

static void test_in_band_config_update_is_transactional(void) {
    static const uint8_t original_au[] = {0, 0, 0, 1, 0x41, 0xe0};
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_codec_config config;
    uint8_t *au;
    uint8_t *original_pointer;
    size_t au_size = sizeof(original_au);

    assert(advc_h264_build_codec_config(&input, 0, &config) == 0);
    au = malloc(au_size);
    assert(au != NULL);
    memcpy(au, original_au, au_size);
    assert(advc_h264_prepend_codec_config(&config, &au, &au_size) == 0);
    assert(au_size == config.size + sizeof(original_au));
    assert(memcmp(au, config.data, config.size) == 0);
    assert(memcmp(au + config.size, original_au, sizeof(original_au)) == 0);
    free(au);

    au_size = sizeof(original_au);
    au = malloc(au_size);
    assert(au != NULL);
    memcpy(au, original_au, au_size);
    original_pointer = au;
    config.size = ADVC_H264_MAX_CONFIG_BYTES + 1u;
    errno = 0;
    assert(advc_h264_prepend_codec_config(&config, &au, &au_size) < 0);
    assert(errno == EINVAL);
    assert(au == original_pointer && au_size == sizeof(original_au));
    assert(memcmp(au, original_au, sizeof(original_au)) == 0);
    free(au);
}

static void test_bounded_reorder_vui(void) {
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_codec_config first;
    struct advc_h264_codec_config second;
    struct advc_h264_codec_config decode_order;

    input.vui_bitstream_restriction_flag = 1;
    input.max_num_reorder_frames = 4;
    input.max_dec_frame_buffering = 4;
    assert(advc_h264_build_codec_config(&input, 0, &first) == 0);
    assert(advc_h264_build_codec_config(&input, 0, &second) == 0);
    assert(first.size > 23);
    assert(first.size == second.size);
    assert(memcmp(first.data, second.data, first.size) == 0);

    input.max_num_reorder_frames = 0;
    assert(advc_h264_build_codec_config(&input, 0, &decode_order) == 0);
    assert(decode_order.size > 23);
    assert(decode_order.size != first.size ||
           memcmp(decode_order.data, first.data, first.size) != 0);

    input.max_num_reorder_frames = 5;
    errno = 0;
    assert(advc_h264_build_codec_config(&input, 0, &first) < 0);
    assert(errno == ENOTSUP);
}

static void test_reference_override_detection(void) {
    static const uint8_t p_uses_pps_defaults[] = {0x41, 0xe0};
    static const uint8_t p_overrides_defaults[] = {0x41, 0xe1};
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_reference_override result;

    input.pic_order_cnt_type = 2;
    input.log2_max_frame_num_minus4 = 0;
    assert(advc_h264_parse_reference_override(
               p_uses_pps_defaults, sizeof(p_uses_pps_defaults), &input,
               &result) == 0);
    assert(result.uses_reference_lists == 1);
    assert(result.override_present == 0);
    assert(advc_h264_parse_reference_override(
               p_overrides_defaults, sizeof(p_overrides_defaults), &input,
               &result) == 0);
    assert(result.uses_reference_lists == 1);
    assert(result.override_present == 1);
}

static void test_fail_closed_subset(void) {
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_codec_config config;

    input.pic_order_cnt_type = 1;
    errno = 0;
    assert(advc_h264_build_codec_config(&input, 0, &config) < 0);
    assert(errno == ENOTSUP);
    input = progressive_1080p();
    input.profile_idc = 100;
    assert(advc_h264_build_codec_config(&input, 0, &config) < 0);
    input = progressive_1080p();
    input.visible_height = 1081;
    assert(advc_h264_build_codec_config(&input, 0, &config) < 0);
    input = progressive_1080p();
    input.second_chroma_qp_index_offset =
        input.chroma_qp_index_offset + 1;
    assert(advc_h264_build_codec_config(&input, 0, &config) < 0);
}

static void test_poc_type_2_subset(void) {
    struct advc_h264_parameter_input input = progressive_1080p();
    struct advc_h264_codec_config first;
    struct advc_h264_codec_config second;

    input.pic_order_cnt_type = 2;
    input.log2_max_pic_order_cnt_lsb_minus4 = 12;
    assert(advc_h264_build_codec_config(&input, 0, &first) == 0);
    /* POC type 2 has no log2_max_pic_order_cnt_lsb_minus4 syntax. */
    input.log2_max_pic_order_cnt_lsb_minus4 = 0;
    assert(advc_h264_build_codec_config(&input, 0, &second) == 0);
    assert(first.size == second.size);
    assert(memcmp(first.data, second.data, first.size) == 0);
}

int main(void) {
    test_slice_identity_and_au();
    test_four_slice_p_access_unit();
    test_deterministic_sps_pps();
    test_in_band_config_update_is_transactional();
    test_bounded_reorder_vui();
    test_reference_override_detection();
    test_poc_type_2_subset();
    test_fail_closed_subset();
    puts("advc H.264 Annex-B reconstruction: PASS");
    return 0;
}
