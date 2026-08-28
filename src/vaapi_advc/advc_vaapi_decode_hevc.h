#ifndef ADVC_VAAPI_DECODE_HEVC_H
#define ADVC_VAAPI_DECODE_HEVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_HEVC_MAX_CONFIG_BYTES 2048u
#define ADVC_HEVC_MAX_SLICES 256u
#define ADVC_HEVC_MAX_TILE_COLUMNS 20u
#define ADVC_HEVC_MAX_TILE_ROWS 22u

/*
 * Neutral copy of the HEVC Main fields needed to reconstruct VPS/SPS/PPS.
 * The VA-API runtime maps VAPictureParameterBufferHEVC into this structure;
 * keeping libva types out of this file makes the converter independently
 * testable.  This is deliberately an 8-bit, 4:2:0, single-layer subset.
 */
struct advc_hevc_parameter_input {
    uint32_t visible_width;
    uint32_t visible_height;
    uint16_t pic_width_in_luma_samples;
    uint16_t pic_height_in_luma_samples;
    uint8_t chroma_format_idc;
    uint8_t separate_colour_plane_flag;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    uint8_t sps_max_dec_pic_buffering_minus1;
    uint8_t no_pic_reordering_flag;
    uint8_t log2_min_luma_coding_block_size_minus3;
    uint8_t log2_diff_max_min_luma_coding_block_size;
    uint8_t log2_min_transform_block_size_minus2;
    uint8_t log2_diff_max_min_transform_block_size;
    uint8_t max_transform_hierarchy_depth_intra;
    uint8_t max_transform_hierarchy_depth_inter;
    uint8_t scaling_list_enabled_flag;
    uint8_t amp_enabled_flag;
    uint8_t sample_adaptive_offset_enabled_flag;
    uint8_t pcm_enabled_flag;
    uint8_t strong_intra_smoothing_enabled_flag;
    uint8_t num_short_term_ref_pic_sets;
    uint8_t long_term_ref_pics_present_flag;
    uint8_t num_long_term_ref_pic_sps;
    uint8_t sps_temporal_mvp_enabled_flag;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;

    uint8_t dependent_slice_segments_enabled_flag;
    uint8_t output_flag_present_flag;
    uint8_t num_extra_slice_header_bits;
    uint8_t sign_data_hiding_enabled_flag;
    uint8_t cabac_init_present_flag;
    uint8_t num_ref_idx_l0_default_active_minus1;
    uint8_t num_ref_idx_l1_default_active_minus1;
    int8_t init_qp_minus26;
    uint8_t constrained_intra_pred_flag;
    uint8_t transform_skip_enabled_flag;
    uint8_t cu_qp_delta_enabled_flag;
    uint8_t diff_cu_qp_delta_depth;
    int8_t pps_cb_qp_offset;
    int8_t pps_cr_qp_offset;
    uint8_t pps_slice_chroma_qp_offsets_present_flag;
    uint8_t weighted_pred_flag;
    uint8_t weighted_bipred_flag;
    uint8_t transquant_bypass_enabled_flag;
    uint8_t tiles_enabled_flag;
    uint8_t entropy_coding_sync_enabled_flag;
    uint8_t num_tile_columns_minus1;
    uint8_t num_tile_rows_minus1;
    uint16_t column_width_minus1[ADVC_HEVC_MAX_TILE_COLUMNS - 1u];
    uint16_t row_height_minus1[ADVC_HEVC_MAX_TILE_ROWS - 1u];
    uint8_t loop_filter_across_tiles_enabled_flag;
    uint8_t pps_loop_filter_across_slices_enabled_flag;
    uint8_t deblocking_filter_override_enabled_flag;
    uint8_t pps_disable_deblocking_filter_flag;
    int8_t pps_beta_offset_div2;
    int8_t pps_tc_offset_div2;
    uint8_t lists_modification_present_flag;
    uint8_t log2_parallel_merge_level_minus2;
    uint8_t slice_segment_header_extension_present_flag;
};

struct advc_hevc_slice_input {
    const uint8_t *data;
    size_t size;
    uint32_t expected_slice_segment_address;
    uint8_t expected_last_slice;
};

struct advc_hevc_slice_identity {
    uint32_t pps_id;
    uint8_t nal_unit_type;
    uint8_t first_slice_segment_in_pic_flag;
    uint8_t no_output_of_prior_pics_flag;
    uint8_t temporal_id_plus1;
};

struct advc_hevc_codec_config {
    uint8_t data[ADVC_HEVC_MAX_CONFIG_BYTES];
    size_t size;
    uint32_t pps_id;
};

/* Parse the syntax preceding slice_pic_parameter_set_id from an original NAL. */
int advc_hevc_parse_slice_identity(
    const uint8_t *data, size_t size,
    struct advc_hevc_slice_identity *identity);

/*
 * Build single-layer Main VPS/SPS/PPS.  Syntax which cannot be recovered
 * exactly from baseline VA-API HEVC buffers (custom scaling lists, SPS RPS,
 * long-term SPS references, PCM) is rejected with ENOTSUP.
 */
int advc_hevc_build_codec_config(
    const struct advc_hevc_parameter_input *input, uint32_t pps_id,
    struct advc_hevc_codec_config *config);

/* Copy complete original VCL NAL units into one normalized Annex-B AU. */
int advc_hevc_build_access_unit(
    const struct advc_hevc_slice_input *slices, size_t slice_count,
    uint8_t **data, size_t *size, uint32_t *pps_id, int *is_irap);

#ifdef __cplusplus
}
#endif

#endif
