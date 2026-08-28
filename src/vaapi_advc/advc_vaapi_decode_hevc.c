#include "advc_vaapi_decode_hevc.h"

#include "advc/protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct bit_writer {
    uint8_t data[ADVC_HEVC_MAX_CONFIG_BYTES];
    size_t bit_count;
};

struct bit_reader {
    uint8_t data[128];
    size_t size;
    size_t bit_offset;
};

static int put_bit(struct bit_writer *writer, unsigned int value) {
    size_t byte_offset;
    unsigned int shift;
    if (writer == NULL || writer->bit_count >= sizeof(writer->data) * 8u) {
        errno = EOVERFLOW;
        return -1;
    }
    byte_offset = writer->bit_count / 8u;
    shift = 7u - (unsigned int)(writer->bit_count % 8u);
    if ((writer->bit_count % 8u) == 0) writer->data[byte_offset] = 0;
    writer->data[byte_offset] |= (uint8_t)((value & 1u) << shift);
    ++writer->bit_count;
    return 0;
}

static int put_bits(struct bit_writer *writer, uint64_t value,
                    unsigned int count) {
    unsigned int i;
    if (count > 64u) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        unsigned int shift = count - i - 1u;
        if (put_bit(writer, (unsigned int)((value >> shift) & 1u)) < 0)
            return -1;
    }
    return 0;
}

static int put_ue(struct bit_writer *writer, uint32_t value) {
    uint64_t code_num = (uint64_t)value + 1u;
    unsigned int bits = 0;
    unsigned int i;
    while ((code_num >> bits) != 0) ++bits;
    for (i = 1; i < bits; ++i)
        if (put_bit(writer, 0) < 0) return -1;
    return put_bits(writer, code_num, bits);
}

static int put_se(struct bit_writer *writer, int32_t value) {
    uint64_t magnitude = value < 0 ? (uint64_t)(-(int64_t)value) :
                                     (uint64_t)value;
    uint64_t code_num = value <= 0 ? magnitude * 2u : magnitude * 2u - 1u;
    if (code_num > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return put_ue(writer, (uint32_t)code_num);
}

static int finish_rbsp(struct bit_writer *writer) {
    if (put_bit(writer, 1) < 0) return -1;
    while ((writer->bit_count % 8u) != 0)
        if (put_bit(writer, 0) < 0) return -1;
    return 0;
}

static int put_profile_tier_level_main(struct bit_writer *writer) {
    /* Main, main-tier, single-layer. Compatibility bit 1 is set. */
    return put_bits(writer, 0, 2) < 0 ||
                   put_bit(writer, 0) < 0 ||
                   put_bits(writer, 1, 5) < 0 ||
                   put_bits(writer, UINT32_C(0x40000000), 32) < 0 ||
                   put_bits(writer, 0, 48) < 0 ||
                   put_bits(writer, 186, 8) < 0
               ? -1 : 0;
}

static int append_annexb_nal(uint8_t *output, size_t capacity, size_t *size,
                             uint8_t nal_unit_type,
                             const struct bit_writer *rbsp) {
    size_t rbsp_size;
    size_t i;
    unsigned int zero_count = 0;
    if (output == NULL || size == NULL || rbsp == NULL ||
        nal_unit_type > 63u || (rbsp->bit_count % 8u) != 0) {
        errno = EINVAL;
        return -1;
    }
    rbsp_size = rbsp->bit_count / 8u;
    if (*size > capacity || capacity - *size < 6u) {
        errno = EOVERFLOW;
        return -1;
    }
    output[(*size)++] = 0;
    output[(*size)++] = 0;
    output[(*size)++] = 0;
    output[(*size)++] = 1;
    output[(*size)++] = (uint8_t)(nal_unit_type << 1);
    output[(*size)++] = 1; /* nuh_layer_id=0, nuh_temporal_id_plus1=1 */
    for (i = 0; i < rbsp_size; ++i) {
        uint8_t byte = rbsp->data[i];
        if (zero_count >= 2u && byte <= 3u) {
            if (*size >= capacity) {
                errno = EOVERFLOW;
                return -1;
            }
            output[(*size)++] = 3;
            zero_count = 0;
        }
        if (*size >= capacity) {
            errno = EOVERFLOW;
            return -1;
        }
        output[(*size)++] = byte;
        if (byte == 0)
            ++zero_count;
        else
            zero_count = 0;
    }
    return 0;
}

static size_t annexb_prefix_size(const uint8_t *data, size_t size) {
    if (size >= 4u && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
        data[3] == 1)
        return 4;
    if (size >= 3u && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 3;
    return 0;
}

static int init_slice_reader(struct bit_reader *reader, const uint8_t *data,
                             size_t size, uint8_t *nal_unit_type,
                             uint8_t *temporal_id_plus1) {
    size_t prefix;
    size_t input;
    unsigned int zero_count = 0;
    uint8_t first;
    uint8_t second;
    unsigned int layer_id;
    if (reader == NULL || data == NULL || nal_unit_type == NULL ||
        temporal_id_plus1 == NULL) {
        errno = EINVAL;
        return -1;
    }
    prefix = annexb_prefix_size(data, size);
    if (size - prefix < 3u) {
        errno = EINVAL;
        return -1;
    }
    first = data[prefix++];
    second = data[prefix++];
    *nal_unit_type = (uint8_t)((first >> 1) & 0x3fu);
    *temporal_id_plus1 = (uint8_t)(second & 7u);
    layer_id = ((unsigned int)(first & 1u) << 5) | (second >> 3);
    if ((first & 0x80u) != 0 || *nal_unit_type > 31u || layer_id != 0 ||
        *temporal_id_plus1 != 1u) {
        errno = ENOTSUP;
        return -1;
    }
    memset(reader, 0, sizeof(*reader));
    for (input = prefix; input < size && reader->size < sizeof(reader->data);
         ++input) {
        uint8_t byte = data[input];
        if (zero_count >= 2u && byte == 3u) {
            zero_count = 0;
            continue;
        }
        reader->data[reader->size++] = byte;
        if (byte == 0)
            ++zero_count;
        else
            zero_count = 0;
    }
    if (reader->size == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int read_bit(struct bit_reader *reader, unsigned int *value) {
    size_t byte_offset;
    unsigned int shift;
    if (reader == NULL || value == NULL ||
        reader->bit_offset >= reader->size * 8u) {
        errno = EPROTO;
        return -1;
    }
    byte_offset = reader->bit_offset / 8u;
    shift = 7u - (unsigned int)(reader->bit_offset % 8u);
    *value = (reader->data[byte_offset] >> shift) & 1u;
    ++reader->bit_offset;
    return 0;
}

static int read_ue(struct bit_reader *reader, uint32_t *value) {
    unsigned int bit;
    unsigned int zeroes = 0;
    uint32_t suffix = 0;
    unsigned int i;
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        if (read_bit(reader, &bit) < 0) return -1;
        if (bit != 0) break;
        if (++zeroes > 31u) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    for (i = 0; i < zeroes; ++i) {
        if (read_bit(reader, &bit) < 0) return -1;
        suffix = (suffix << 1) | bit;
    }
    *value = ((UINT32_C(1) << zeroes) - 1u) + suffix;
    return 0;
}

int advc_hevc_parse_slice_identity(
    const uint8_t *data, size_t size,
    struct advc_hevc_slice_identity *identity) {
    struct bit_reader reader;
    unsigned int bit;
    uint8_t nal_type;
    uint8_t temporal_id_plus1;
    if (identity == NULL ||
        init_slice_reader(&reader, data, size, &nal_type,
                          &temporal_id_plus1) < 0)
        return -1;
    memset(identity, 0, sizeof(*identity));
    identity->nal_unit_type = nal_type;
    identity->temporal_id_plus1 = temporal_id_plus1;
    if (read_bit(&reader, &bit) < 0) return -1;
    identity->first_slice_segment_in_pic_flag = (uint8_t)bit;
    if (nal_type >= 16u && nal_type <= 23u) {
        if (read_bit(&reader, &bit) < 0) return -1;
        identity->no_output_of_prior_pics_flag = (uint8_t)bit;
    }
    if (read_ue(&reader, &identity->pps_id) < 0) return -1;
    if (identity->pps_id > 63u) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int validate_parameters(const struct advc_hevc_parameter_input *input,
                               uint32_t *crop_right,
                               uint32_t *crop_bottom,
                               uint32_t *ctb_width,
                               uint32_t *ctb_height) {
    uint32_t min_cb_log2;
    uint32_t ctb_log2;
    uint32_t ctb_size;
    uint32_t sum;
    unsigned int i;
    if (input == NULL || crop_right == NULL || crop_bottom == NULL ||
        ctb_width == NULL || ctb_height == NULL ||
        input->visible_width == 0 || input->visible_height == 0 ||
        input->visible_width > 8192u || input->visible_height > 8192u ||
        input->pic_width_in_luma_samples == 0 ||
        input->pic_height_in_luma_samples == 0) {
        errno = EINVAL;
        return -1;
    }
    if (input->chroma_format_idc != 1u ||
        input->separate_colour_plane_flag != 0 ||
        input->bit_depth_luma_minus8 != 0 ||
        input->bit_depth_chroma_minus8 != 0 ||
        input->scaling_list_enabled_flag != 0 ||
        input->pcm_enabled_flag != 0 ||
        input->num_short_term_ref_pic_sets != 0 ||
        input->long_term_ref_pics_present_flag != 0 ||
        input->num_long_term_ref_pic_sps != 0 ||
        input->log2_max_pic_order_cnt_lsb_minus4 > 12u ||
        input->log2_min_luma_coding_block_size_minus3 > 3u ||
        input->log2_diff_max_min_luma_coding_block_size > 3u ||
        input->log2_min_transform_block_size_minus2 > 3u ||
        input->log2_diff_max_min_transform_block_size > 3u ||
        input->sps_max_dec_pic_buffering_minus1 > 15u ||
        input->num_ref_idx_l0_default_active_minus1 > 14u ||
        input->num_ref_idx_l1_default_active_minus1 > 14u ||
        input->num_extra_slice_header_bits > 7u ||
        input->num_tile_columns_minus1 >= ADVC_HEVC_MAX_TILE_COLUMNS ||
        input->num_tile_rows_minus1 >= ADVC_HEVC_MAX_TILE_ROWS) {
        errno = ENOTSUP;
        return -1;
    }
    if (input->visible_width > input->pic_width_in_luma_samples ||
        input->visible_height > input->pic_height_in_luma_samples ||
        ((input->pic_width_in_luma_samples - input->visible_width) & 1u) != 0 ||
        ((input->pic_height_in_luma_samples - input->visible_height) & 1u) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    *crop_right =
        (input->pic_width_in_luma_samples - input->visible_width) / 2u;
    *crop_bottom =
        (input->pic_height_in_luma_samples - input->visible_height) / 2u;
    min_cb_log2 = input->log2_min_luma_coding_block_size_minus3 + 3u;
    ctb_log2 = min_cb_log2 +
               input->log2_diff_max_min_luma_coding_block_size;
    if (ctb_log2 > 6u) {
        errno = ENOTSUP;
        return -1;
    }
    ctb_size = UINT32_C(1) << ctb_log2;
    *ctb_width = (input->pic_width_in_luma_samples + ctb_size - 1u) /
                 ctb_size;
    *ctb_height = (input->pic_height_in_luma_samples + ctb_size - 1u) /
                  ctb_size;
    if (input->tiles_enabled_flag == 0) {
        if (input->num_tile_columns_minus1 != 0 ||
            input->num_tile_rows_minus1 != 0) {
            errno = ENOTSUP;
            return -1;
        }
        return 0;
    }
    if ((uint32_t)input->num_tile_columns_minus1 + 1u > *ctb_width ||
        (uint32_t)input->num_tile_rows_minus1 + 1u > *ctb_height) {
        errno = ENOTSUP;
        return -1;
    }
    sum = 0;
    for (i = 0; i < input->num_tile_columns_minus1; ++i) {
        sum += (uint32_t)input->column_width_minus1[i] + 1u;
        if (sum >= *ctb_width) {
            errno = ENOTSUP;
            return -1;
        }
    }
    sum = 0;
    for (i = 0; i < input->num_tile_rows_minus1; ++i) {
        sum += (uint32_t)input->row_height_minus1[i] + 1u;
        if (sum >= *ctb_height) {
            errno = ENOTSUP;
            return -1;
        }
    }
    return 0;
}

int advc_hevc_build_codec_config(
    const struct advc_hevc_parameter_input *input, uint32_t pps_id,
    struct advc_hevc_codec_config *config) {
    struct bit_writer vps;
    struct bit_writer sps;
    struct bit_writer pps;
    uint32_t crop_right;
    uint32_t crop_bottom;
    uint32_t ctb_width;
    uint32_t ctb_height;
    uint32_t max_reorder;
    unsigned int i;
    if (config == NULL || pps_id > 63u ||
        validate_parameters(input, &crop_right, &crop_bottom, &ctb_width,
                            &ctb_height) < 0)
        return -1;
    (void)ctb_width;
    (void)ctb_height;
    memset(config, 0, sizeof(*config));
    memset(&vps, 0, sizeof(vps));
    memset(&sps, 0, sizeof(sps));
    memset(&pps, 0, sizeof(pps));
    max_reorder = input->no_pic_reordering_flag ? 0u :
                  input->sps_max_dec_pic_buffering_minus1;

    if (put_bits(&vps, 0, 4) < 0 || put_bit(&vps, 1) < 0 ||
        put_bit(&vps, 1) < 0 || put_bits(&vps, 0, 6) < 0 ||
        put_bits(&vps, 0, 3) < 0 || put_bit(&vps, 1) < 0 ||
        put_bits(&vps, UINT16_MAX, 16) < 0 ||
        put_profile_tier_level_main(&vps) < 0 ||
        put_bit(&vps, 0) < 0 ||
        put_ue(&vps, input->sps_max_dec_pic_buffering_minus1) < 0 ||
        put_ue(&vps, max_reorder) < 0 || put_ue(&vps, 0) < 0 ||
        put_bits(&vps, 0, 6) < 0 || put_ue(&vps, 0) < 0 ||
        put_bit(&vps, 0) < 0 || put_bit(&vps, 0) < 0 ||
        finish_rbsp(&vps) < 0)
        return -1;

    if (put_bits(&sps, 0, 4) < 0 || put_bits(&sps, 0, 3) < 0 ||
        put_bit(&sps, 1) < 0 || put_profile_tier_level_main(&sps) < 0 ||
        put_ue(&sps, 0) < 0 || put_ue(&sps, input->chroma_format_idc) < 0 ||
        put_ue(&sps, input->pic_width_in_luma_samples) < 0 ||
        put_ue(&sps, input->pic_height_in_luma_samples) < 0 ||
        put_bit(&sps, crop_right != 0 || crop_bottom != 0) < 0)
        return -1;
    if ((crop_right != 0 || crop_bottom != 0) &&
        (put_ue(&sps, 0) < 0 || put_ue(&sps, crop_right) < 0 ||
         put_ue(&sps, 0) < 0 || put_ue(&sps, crop_bottom) < 0))
        return -1;
    if (put_ue(&sps, input->bit_depth_luma_minus8) < 0 ||
        put_ue(&sps, input->bit_depth_chroma_minus8) < 0 ||
        put_ue(&sps, input->log2_max_pic_order_cnt_lsb_minus4) < 0 ||
        put_bit(&sps, 0) < 0 ||
        put_ue(&sps, input->sps_max_dec_pic_buffering_minus1) < 0 ||
        put_ue(&sps, max_reorder) < 0 || put_ue(&sps, 0) < 0 ||
        put_ue(&sps, input->log2_min_luma_coding_block_size_minus3) < 0 ||
        put_ue(&sps,
               input->log2_diff_max_min_luma_coding_block_size) < 0 ||
        put_ue(&sps, input->log2_min_transform_block_size_minus2) < 0 ||
        put_ue(&sps,
               input->log2_diff_max_min_transform_block_size) < 0 ||
        put_ue(&sps, input->max_transform_hierarchy_depth_inter) < 0 ||
        put_ue(&sps, input->max_transform_hierarchy_depth_intra) < 0 ||
        put_bit(&sps, input->scaling_list_enabled_flag) < 0 ||
        put_bit(&sps, input->amp_enabled_flag) < 0 ||
        put_bit(&sps, input->sample_adaptive_offset_enabled_flag) < 0 ||
        put_bit(&sps, input->pcm_enabled_flag) < 0 ||
        put_ue(&sps, input->num_short_term_ref_pic_sets) < 0 ||
        put_bit(&sps, input->long_term_ref_pics_present_flag) < 0 ||
        put_bit(&sps, input->sps_temporal_mvp_enabled_flag) < 0 ||
        put_bit(&sps, input->strong_intra_smoothing_enabled_flag) < 0 ||
        put_bit(&sps, 0) < 0 || /* vui_parameters_present_flag */
        put_bit(&sps, 0) < 0 || /* sps_extension_present_flag */
        finish_rbsp(&sps) < 0)
        return -1;

    if (put_ue(&pps, pps_id) < 0 || put_ue(&pps, 0) < 0 ||
        put_bit(&pps, input->dependent_slice_segments_enabled_flag) < 0 ||
        put_bit(&pps, input->output_flag_present_flag) < 0 ||
        put_bits(&pps, input->num_extra_slice_header_bits, 3) < 0 ||
        put_bit(&pps, input->sign_data_hiding_enabled_flag) < 0 ||
        put_bit(&pps, input->cabac_init_present_flag) < 0 ||
        put_ue(&pps, input->num_ref_idx_l0_default_active_minus1) < 0 ||
        put_ue(&pps, input->num_ref_idx_l1_default_active_minus1) < 0 ||
        put_se(&pps, input->init_qp_minus26) < 0 ||
        put_bit(&pps, input->constrained_intra_pred_flag) < 0 ||
        put_bit(&pps, input->transform_skip_enabled_flag) < 0 ||
        put_bit(&pps, input->cu_qp_delta_enabled_flag) < 0)
        return -1;
    if (input->cu_qp_delta_enabled_flag != 0 &&
        put_ue(&pps, input->diff_cu_qp_delta_depth) < 0)
        return -1;
    if (put_se(&pps, input->pps_cb_qp_offset) < 0 ||
        put_se(&pps, input->pps_cr_qp_offset) < 0 ||
        put_bit(&pps,
                input->pps_slice_chroma_qp_offsets_present_flag) < 0 ||
        put_bit(&pps, input->weighted_pred_flag) < 0 ||
        put_bit(&pps, input->weighted_bipred_flag) < 0 ||
        put_bit(&pps, input->transquant_bypass_enabled_flag) < 0 ||
        put_bit(&pps, input->tiles_enabled_flag) < 0 ||
        put_bit(&pps, input->entropy_coding_sync_enabled_flag) < 0)
        return -1;
    if (input->tiles_enabled_flag != 0) {
        if (put_ue(&pps, input->num_tile_columns_minus1) < 0 ||
            put_ue(&pps, input->num_tile_rows_minus1) < 0 ||
            put_bit(&pps, 0) < 0)
            return -1; /* explicit column/row widths */
        for (i = 0; i < input->num_tile_columns_minus1; ++i)
            if (put_ue(&pps, input->column_width_minus1[i]) < 0) return -1;
        for (i = 0; i < input->num_tile_rows_minus1; ++i)
            if (put_ue(&pps, input->row_height_minus1[i]) < 0) return -1;
        if (put_bit(&pps, input->loop_filter_across_tiles_enabled_flag) < 0)
            return -1;
    }
    if (put_bit(&pps,
                input->pps_loop_filter_across_slices_enabled_flag) < 0 ||
        put_bit(&pps, 1) < 0 || /* deblocking_filter_control_present_flag */
        put_bit(&pps, input->deblocking_filter_override_enabled_flag) < 0 ||
        put_bit(&pps, input->pps_disable_deblocking_filter_flag) < 0)
        return -1;
    if (input->pps_disable_deblocking_filter_flag == 0 &&
        (put_se(&pps, input->pps_beta_offset_div2) < 0 ||
         put_se(&pps, input->pps_tc_offset_div2) < 0))
        return -1;
    if (put_bit(&pps, 0) < 0 || /* pps_scaling_list_data_present_flag */
        put_bit(&pps, input->lists_modification_present_flag) < 0 ||
        put_ue(&pps, input->log2_parallel_merge_level_minus2) < 0 ||
        put_bit(&pps,
                input->slice_segment_header_extension_present_flag) < 0 ||
        put_bit(&pps, 0) < 0 || /* pps_extension_present_flag */
        finish_rbsp(&pps) < 0)
        return -1;

    if (append_annexb_nal(config->data, sizeof(config->data), &config->size,
                          32, &vps) < 0 ||
        append_annexb_nal(config->data, sizeof(config->data), &config->size,
                          33, &sps) < 0 ||
        append_annexb_nal(config->data, sizeof(config->data), &config->size,
                          34, &pps) < 0)
        return -1;
    config->pps_id = pps_id;
    return 0;
}

int advc_hevc_build_access_unit(
    const struct advc_hevc_slice_input *slices, size_t slice_count,
    uint8_t **data, size_t *size, uint32_t *pps_id, int *is_irap) {
    uint8_t *output;
    size_t output_size = 0;
    size_t i;
    uint32_t common_pps = 0;
    int any_irap = 0;
    if (slices == NULL || data == NULL || size == NULL || pps_id == NULL ||
        is_irap == NULL || slice_count == 0 ||
        slice_count > ADVC_HEVC_MAX_SLICES) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < slice_count; ++i) {
        struct advc_hevc_slice_identity identity;
        size_t prefix;
        if (slices[i].data == NULL || slices[i].size == 0 ||
            advc_hevc_parse_slice_identity(slices[i].data, slices[i].size,
                                           &identity) < 0)
            return -1;
        if ((slices[i].expected_slice_segment_address == 0) !=
            (identity.first_slice_segment_in_pic_flag != 0)) {
            errno = EPROTO;
            return -1;
        }
        if ((i + 1u == slice_count) !=
            (slices[i].expected_last_slice != 0)) {
            errno = EPROTO;
            return -1;
        }
        if (i == 0) {
            if (!identity.first_slice_segment_in_pic_flag) {
                errno = EPROTO;
                return -1;
            }
            common_pps = identity.pps_id;
        } else if (identity.pps_id != common_pps ||
                   identity.first_slice_segment_in_pic_flag) {
            errno = ENOTSUP;
            return -1;
        }
        prefix = annexb_prefix_size(slices[i].data, slices[i].size);
        if (output_size > ADVC_MAX_INPUT_BYTES - 4u ||
            slices[i].size - prefix >
                ADVC_MAX_INPUT_BYTES - output_size - 4u) {
            errno = EOVERFLOW;
            return -1;
        }
        output_size += 4u + slices[i].size - prefix;
        any_irap |= identity.nal_unit_type >= 16u &&
                    identity.nal_unit_type <= 23u;
    }
    output = malloc(output_size);
    if (output == NULL) return -1;
    output_size = 0;
    for (i = 0; i < slice_count; ++i) {
        size_t prefix = annexb_prefix_size(slices[i].data, slices[i].size);
        output[output_size++] = 0;
        output[output_size++] = 0;
        output[output_size++] = 0;
        output[output_size++] = 1;
        memcpy(output + output_size, slices[i].data + prefix,
               slices[i].size - prefix);
        output_size += slices[i].size - prefix;
    }
    *data = output;
    *size = output_size;
    *pps_id = common_pps;
    *is_irap = any_irap;
    return 0;
}
