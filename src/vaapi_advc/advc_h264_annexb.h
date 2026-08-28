#ifndef ADVC_H264_ANNEXB_H
#define ADVC_H264_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_H264_MAX_CONFIG_BYTES 1024u
#define ADVC_H264_MAX_SLICES 256u

struct advc_h264_parameter_input {
    uint8_t profile_idc; /* Supported: constrained baseline (66) or main (77). */
    uint32_t visible_width;
    uint32_t visible_height;
    uint16_t picture_width_in_mbs_minus1;
    uint16_t picture_height_in_mbs_minus1;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    uint8_t chroma_format_idc;
    uint8_t separate_colour_plane_flag;
    uint8_t gaps_in_frame_num_value_allowed_flag;
    uint8_t frame_mbs_only_flag;
    uint8_t mb_adaptive_frame_field_flag;
    uint8_t direct_8x8_inference_flag;
    uint8_t log2_max_frame_num_minus4;
    uint8_t pic_order_cnt_type;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;
    uint8_t delta_pic_order_always_zero_flag;
    uint8_t num_ref_frames;
    uint8_t vui_bitstream_restriction_flag;
    uint8_t max_num_reorder_frames;
    uint8_t max_dec_frame_buffering;
    int8_t pic_init_qp_minus26;
    int8_t pic_init_qs_minus26;
    int8_t chroma_qp_index_offset;
    int8_t second_chroma_qp_index_offset;
    uint8_t entropy_coding_mode_flag;
    uint8_t weighted_pred_flag;
    uint8_t weighted_bipred_idc;
    uint8_t transform_8x8_mode_flag;
    uint8_t field_pic_flag;
    uint8_t constrained_intra_pred_flag;
    uint8_t bottom_field_pic_order_in_frame_present_flag;
    uint8_t deblocking_filter_control_present_flag;
    uint8_t redundant_pic_cnt_present_flag;
    uint8_t num_ref_idx_l0_default_active_minus1;
    uint8_t num_ref_idx_l1_default_active_minus1;
};

struct advc_h264_slice_input {
    const uint8_t *data;
    size_t size;
    uint16_t expected_first_mb_in_slice;
    uint8_t expected_slice_type;
};

struct advc_h264_codec_config {
    uint8_t data[ADVC_H264_MAX_CONFIG_BYTES];
    size_t size;
    uint32_t pps_id;
};

struct advc_h264_reference_override {
    uint8_t uses_reference_lists;
    uint8_t uses_list1;
    uint8_t override_present;
};

/* Parses the first three slice_header ue(v) fields from original EBSP bytes. */
int advc_h264_parse_slice_identity(const uint8_t *data, size_t size,
                                   uint32_t *first_mb_in_slice,
                                   uint32_t *slice_type, uint32_t *pps_id,
                                   int *is_idr);

/*
 * Locates num_ref_idx_active_override_flag in the original slice header for
 * the supported progressive subset. When the flag is absent, VA's effective
 * ref counts are the exact PPS defaults; when present, they must not be used
 * to rewrite PPS defaults.
 */
int advc_h264_parse_reference_override(
    const uint8_t *data, size_t size,
    const struct advc_h264_parameter_input *parameters,
    struct advc_h264_reference_override *result);

/*
 * Creates Annex-B SPS/PPS for the deliberately bounded 8-bit progressive
 * 4:2:0 subset. Unsupported syntax (interlace, POC type 1, High scaling
 * matrices, FMO) fails with ENOTSUP instead of being guessed.
 */
int advc_h264_build_codec_config(
    const struct advc_h264_parameter_input *input, uint32_t pps_id,
    struct advc_h264_codec_config *config);

/* Copies complete original VCL NAL units into one normalized Annex-B AU. */
int advc_h264_build_access_unit(const struct advc_h264_slice_input *slices,
                                size_t slice_count, uint8_t **data,
                                size_t *size, uint32_t *pps_id, int *is_idr);

/*
 * Atomically prefixes a regenerated Annex-B SPS/PPS pair to an existing
 * access unit.  This is used when the first inter picture reveals PPS
 * defaults that VA-API did not expose on the preceding IDR picture.  Sending
 * the update in-band with that picture avoids a mid-stream MediaCodec CSD
 * transaction while preserving exact decoder ordering.
 */
int advc_h264_prepend_codec_config(
    const struct advc_h264_codec_config *config, uint8_t **data,
    size_t *size);

#ifdef __cplusplus
}
#endif

#endif
