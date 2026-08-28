#include "advc_vaapi_decode_hevc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct advc_hevc_parameter_input progressive_1080p(void) {
    struct advc_hevc_parameter_input input;
    memset(&input, 0, sizeof(input));
    input.visible_width = 1920;
    input.visible_height = 1080;
    input.pic_width_in_luma_samples = 1920;
    input.pic_height_in_luma_samples = 1088;
    input.chroma_format_idc = 1;
    input.sps_max_dec_pic_buffering_minus1 = 4;
    input.log2_min_luma_coding_block_size_minus3 = 0;
    input.log2_diff_max_min_luma_coding_block_size = 3;
    input.log2_min_transform_block_size_minus2 = 0;
    input.log2_diff_max_min_transform_block_size = 3;
    input.max_transform_hierarchy_depth_intra = 2;
    input.max_transform_hierarchy_depth_inter = 2;
    input.amp_enabled_flag = 1;
    input.sample_adaptive_offset_enabled_flag = 1;
    input.strong_intra_smoothing_enabled_flag = 1;
    input.log2_max_pic_order_cnt_lsb_minus4 = 4;
    input.sign_data_hiding_enabled_flag = 1;
    input.num_ref_idx_l0_default_active_minus1 = 1;
    input.init_qp_minus26 = -2;
    input.pps_loop_filter_across_slices_enabled_flag = 1;
    input.log2_parallel_merge_level_minus2 = 0;
    return input;
}

static size_t find_nal_type(const uint8_t *data, size_t size,
                            uint8_t wanted) {
    size_t i;
    for (i = 0; i + 6u <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
            data[i + 3] == 1 && ((data[i + 4] >> 1) & 0x3fu) == wanted &&
            data[i + 5] == 1)
            return i;
    }
    return SIZE_MAX;
}

static void test_slice_identity_and_au(void) {
    static const uint8_t idr[] = {0x26, 0x01, 0xa0};
    static const uint8_t idr_prefixed[] = {
        0x00, 0x00, 0x01, 0x26, 0x01, 0xa0,
    };
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xa0,
    };
    struct advc_hevc_slice_identity identity;
    struct advc_hevc_slice_input slice;
    uint8_t *au = NULL;
    size_t au_size = 0;
    uint32_t pps_id = UINT32_MAX;
    int is_irap = 0;

    assert(advc_hevc_parse_slice_identity(idr, sizeof(idr), &identity) == 0);
    assert(identity.nal_unit_type == 19);
    assert(identity.first_slice_segment_in_pic_flag == 1);
    assert(identity.no_output_of_prior_pics_flag == 0);
    assert(identity.pps_id == 0);
    assert(identity.temporal_id_plus1 == 1);

    memset(&slice, 0, sizeof(slice));
    slice.data = idr_prefixed;
    slice.size = sizeof(idr_prefixed);
    slice.expected_last_slice = 1;
    assert(advc_hevc_build_access_unit(&slice, 1, &au, &au_size, &pps_id,
                                       &is_irap) == 0);
    assert(au_size == sizeof(expected));
    assert(memcmp(au, expected, sizeof(expected)) == 0);
    assert(pps_id == 0 && is_irap == 1);
    free(au);
}

static void test_codec_config(void) {
    struct advc_hevc_parameter_input input = progressive_1080p();
    struct advc_hevc_codec_config first;
    struct advc_hevc_codec_config second;

    assert(advc_hevc_build_codec_config(&input, 3, &first) == 0);
    assert(first.pps_id == 3);
    assert(find_nal_type(first.data, first.size, 32) != SIZE_MAX);
    assert(find_nal_type(first.data, first.size, 33) != SIZE_MAX);
    assert(find_nal_type(first.data, first.size, 34) != SIZE_MAX);
    assert(advc_hevc_build_codec_config(&input, 3, &second) == 0);
    assert(first.size == second.size);
    assert(memcmp(first.data, second.data, first.size) == 0);
}

static void test_tiles(void) {
    struct advc_hevc_parameter_input input = progressive_1080p();
    struct advc_hevc_codec_config config;
    input.tiles_enabled_flag = 1;
    input.num_tile_columns_minus1 = 1;
    input.num_tile_rows_minus1 = 1;
    input.column_width_minus1[0] = 14;
    input.row_height_minus1[0] = 7;
    input.loop_filter_across_tiles_enabled_flag = 1;
    assert(advc_hevc_build_codec_config(&input, 0, &config) == 0);

    input.column_width_minus1[0] = 29;
    errno = 0;
    assert(advc_hevc_build_codec_config(&input, 0, &config) < 0);
    assert(errno == ENOTSUP);
}

static void test_live_validated_config_vector(void) {
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01,
        0xff, 0xff, 0x01, 0x40, 0x00, 0x00, 0x03, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
        0x00, 0xba, 0x3c, 0x09, 0x00, 0x00, 0x00, 0x01,
        0x42, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0x03,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x03, 0x00, 0xba, 0xa0, 0x0a, 0x08, 0x0f, 0x16,
        0x53, 0xd2, 0x93, 0x0b, 0x20, 0x00, 0x00, 0x00,
        0x01, 0x44, 0x01, 0xc0, 0x73, 0xc0, 0xcc, 0x90,
    };
    struct advc_hevc_parameter_input input;
    struct advc_hevc_codec_config config;
    memset(&input, 0, sizeof(input));
    input.visible_width = 320;
    input.visible_height = 240;
    input.pic_width_in_luma_samples = 320;
    input.pic_height_in_luma_samples = 240;
    input.chroma_format_idc = 1;
    input.sps_max_dec_pic_buffering_minus1 = 2;
    input.no_pic_reordering_flag = 1;
    input.log2_min_luma_coding_block_size_minus3 = 1;
    input.log2_diff_max_min_luma_coding_block_size = 1;
    input.log2_diff_max_min_transform_block_size = 3;
    input.sps_temporal_mvp_enabled_flag = 1;
    input.strong_intra_smoothing_enabled_flag = 1;
    input.log2_max_pic_order_cnt_lsb_minus4 = 4;
    input.cu_qp_delta_enabled_flag = 1;
    input.pps_loop_filter_across_slices_enabled_flag = 1;
    assert(advc_hevc_build_codec_config(&input, 0, &config) == 0);
    assert(config.size == sizeof(expected));
    assert(memcmp(config.data, expected, sizeof(expected)) == 0);
}

static void test_fail_closed_subset(void) {
    struct advc_hevc_parameter_input input = progressive_1080p();
    struct advc_hevc_codec_config config;
    static const uint8_t temporal_layer_slice[] = {0x26, 0x02, 0xa0};
    struct advc_hevc_slice_identity identity;

    input.scaling_list_enabled_flag = 1;
    errno = 0;
    assert(advc_hevc_build_codec_config(&input, 0, &config) < 0);
    assert(errno == ENOTSUP);

    input = progressive_1080p();
    input.num_short_term_ref_pic_sets = 1;
    assert(advc_hevc_build_codec_config(&input, 0, &config) < 0);

    input = progressive_1080p();
    input.long_term_ref_pics_present_flag = 1;
    assert(advc_hevc_build_codec_config(&input, 0, &config) < 0);

    errno = 0;
    assert(advc_hevc_parse_slice_identity(temporal_layer_slice,
                                          sizeof(temporal_layer_slice),
                                          &identity) < 0);
    assert(errno == ENOTSUP);
}

int main(void) {
    test_slice_identity_and_au();
    test_codec_config();
    test_tiles();
    test_live_validated_config_vector();
    test_fail_closed_subset();
    puts("advc HEVC Main Annex-B reconstruction: PASS");
    return 0;
}
