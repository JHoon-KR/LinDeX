#include "advc_h264_annexb.h"

#include "advc/protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct bit_writer {
    uint8_t data[ADVC_H264_MAX_CONFIG_BYTES];
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

static int put_bits(struct bit_writer *writer, uint32_t value,
                    unsigned int count) {
    unsigned int i;

    if (count > 32u) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        unsigned int shift = count - i - 1u;
        if (put_bit(writer, (value >> shift) & 1u) < 0) return -1;
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
    return put_bits(writer, (uint32_t)code_num, bits);
}

static int put_se(struct bit_writer *writer, int32_t value) {
    uint64_t magnitude;
    uint64_t code_num;

    magnitude = value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
    code_num = value <= 0 ? magnitude * 2u : magnitude * 2u - 1u;
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

static int append_annexb_nal(uint8_t *output, size_t output_capacity,
                             size_t *output_size, uint8_t nal_header,
                             const struct bit_writer *rbsp) {
    size_t rbsp_size;
    size_t i;
    unsigned int zero_count = 0;

    if (output == NULL || output_size == NULL || rbsp == NULL ||
        (rbsp->bit_count % 8u) != 0) {
        errno = EINVAL;
        return -1;
    }
    rbsp_size = rbsp->bit_count / 8u;
    if (*output_size > output_capacity ||
        output_capacity - *output_size < 5u) {
        errno = EOVERFLOW;
        return -1;
    }
    output[(*output_size)++] = 0;
    output[(*output_size)++] = 0;
    output[(*output_size)++] = 0;
    output[(*output_size)++] = 1;
    output[(*output_size)++] = nal_header;
    for (i = 0; i < rbsp_size; ++i) {
        uint8_t byte = rbsp->data[i];
        if (zero_count >= 2u && byte <= 3u) {
            if (*output_size >= output_capacity) {
                errno = EOVERFLOW;
                return -1;
            }
            output[(*output_size)++] = 3;
            zero_count = 0;
        }
        if (*output_size >= output_capacity) {
            errno = EOVERFLOW;
            return -1;
        }
        output[(*output_size)++] = byte;
        if (byte == 0)
            ++zero_count;
        else
            zero_count = 0;
    }
    return 0;
}

static size_t annexb_prefix_size(const uint8_t *data, size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
        data[3] == 1)
        return 4;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 3;
    return 0;
}

static int init_slice_reader(struct bit_reader *reader, const uint8_t *data,
                             size_t size, uint8_t *nal_header) {
    size_t prefix;
    size_t input;
    unsigned int zero_count = 0;

    if (reader == NULL || data == NULL || nal_header == NULL) {
        errno = EINVAL;
        return -1;
    }
    prefix = annexb_prefix_size(data, size);
    if (prefix >= size) {
        errno = EINVAL;
        return -1;
    }
    *nal_header = data[prefix++];
    if ((*nal_header & 0x80u) != 0 ||
        ((*nal_header & 0x1fu) != 1u && (*nal_header & 0x1fu) != 5u)) {
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

static int read_bits(struct bit_reader *reader, unsigned int count,
                     uint32_t *value) {
    unsigned int bit;
    unsigned int i;
    uint32_t result = 0;
    if (reader == NULL || value == NULL || count > 32u) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (read_bit(reader, &bit) < 0) return -1;
        result = (result << 1) | bit;
    }
    *value = result;
    return 0;
}

static int read_se(struct bit_reader *reader, int32_t *value) {
    uint32_t code_num;
    if (value == NULL || read_ue(reader, &code_num) < 0) return -1;
    if (code_num > (uint32_t)INT32_MAX * 2u) {
        errno = EOVERFLOW;
        return -1;
    }
    *value = (code_num & 1u) != 0 ? (int32_t)((code_num + 1u) / 2u) :
                                   -(int32_t)(code_num / 2u);
    return 0;
}

int advc_h264_parse_slice_identity(const uint8_t *data, size_t size,
                                   uint32_t *first_mb_in_slice,
                                   uint32_t *slice_type, uint32_t *pps_id,
                                   int *is_idr) {
    struct bit_reader reader;
    uint8_t nal_header;

    if (first_mb_in_slice == NULL || slice_type == NULL || pps_id == NULL ||
        is_idr == NULL ||
        init_slice_reader(&reader, data, size, &nal_header) < 0 ||
        read_ue(&reader, first_mb_in_slice) < 0 ||
        read_ue(&reader, slice_type) < 0 || read_ue(&reader, pps_id) < 0)
        return -1;
    if (*slice_type > 9u || *pps_id > 255u) {
        errno = ENOTSUP;
        return -1;
    }
    *is_idr = (nal_header & 0x1fu) == 5u;
    return 0;
}

int advc_h264_parse_reference_override(
    const uint8_t *data, size_t size,
    const struct advc_h264_parameter_input *parameters,
    struct advc_h264_reference_override *result) {
    struct bit_reader reader;
    uint8_t nal_header;
    uint32_t ignored;
    uint32_t slice_type;
    unsigned int flag;
    int32_t ignored_signed;
    int is_idr;
    unsigned int normalized_type;

    if (data == NULL || parameters == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (parameters->frame_mbs_only_flag != 1u ||
        parameters->field_pic_flag != 0 ||
        (parameters->pic_order_cnt_type != 0u &&
         parameters->pic_order_cnt_type != 2u) ||
        parameters->log2_max_frame_num_minus4 > 12u ||
        parameters->log2_max_pic_order_cnt_lsb_minus4 > 12u) {
        errno = ENOTSUP;
        return -1;
    }
    if (init_slice_reader(&reader, data, size, &nal_header) < 0 ||
        read_ue(&reader, &ignored) < 0 || read_ue(&reader, &slice_type) < 0 ||
        read_ue(&reader, &ignored) < 0)
        return -1;
    if (slice_type > 9u) {
        errno = ENOTSUP;
        return -1;
    }
    if (read_bits(&reader, parameters->log2_max_frame_num_minus4 + 4u,
                  &ignored) < 0) {
        return -1;
    }
    is_idr = (nal_header & 0x1fu) == 5u;
    if (is_idr && read_ue(&reader, &ignored) < 0) return -1;
    if (parameters->pic_order_cnt_type == 0u) {
        if (read_bits(&reader,
                      parameters->log2_max_pic_order_cnt_lsb_minus4 + 4u,
                      &ignored) < 0)
            return -1;
        if (parameters->bottom_field_pic_order_in_frame_present_flag != 0 &&
            read_se(&reader, &ignored_signed) < 0)
            return -1;
    }
    if (parameters->redundant_pic_cnt_present_flag != 0 &&
        read_ue(&reader, &ignored) < 0)
        return -1;

    memset(result, 0, sizeof(*result));
    normalized_type = (unsigned int)(slice_type % 5u);
    if (normalized_type == 1u && read_bit(&reader, &flag) < 0) return -1;
    if (normalized_type == 0u || normalized_type == 1u ||
        normalized_type == 3u) {
        result->uses_reference_lists = 1;
        result->uses_list1 = normalized_type == 1u;
        if (read_bit(&reader, &flag) < 0) return -1;
        result->override_present = (uint8_t)flag;
    }
    return 0;
}

static int validate_parameters(const struct advc_h264_parameter_input *input) {
    uint32_t coded_width;
    uint32_t coded_height;

    if (input == NULL || input->visible_width == 0 ||
        input->visible_height == 0 || input->visible_width > 8192u ||
        input->visible_height > 8192u) {
        errno = EINVAL;
        return -1;
    }
    if ((input->profile_idc != 66u && input->profile_idc != 77u) ||
        input->bit_depth_luma_minus8 != 0 ||
        input->bit_depth_chroma_minus8 != 0 ||
        input->chroma_format_idc != 1u ||
        input->separate_colour_plane_flag != 0 ||
        input->frame_mbs_only_flag != 1u ||
        input->mb_adaptive_frame_field_flag != 0 ||
        input->field_pic_flag != 0 ||
        (input->pic_order_cnt_type != 0 && input->pic_order_cnt_type != 2) ||
        input->transform_8x8_mode_flag != 0 ||
        input->second_chroma_qp_index_offset !=
            input->chroma_qp_index_offset ||
        input->weighted_bipred_idc > 2u ||
        input->log2_max_frame_num_minus4 > 12u ||
        input->log2_max_pic_order_cnt_lsb_minus4 > 12u ||
        input->num_ref_frames > 16u ||
        (input->vui_bitstream_restriction_flag != 0 &&
         (input->max_dec_frame_buffering > 16u ||
          input->max_num_reorder_frames >
              input->max_dec_frame_buffering ||
          input->max_dec_frame_buffering < input->num_ref_frames)) ||
        (input->vui_bitstream_restriction_flag == 0 &&
         (input->max_num_reorder_frames != 0 ||
          input->max_dec_frame_buffering != 0))) {
        errno = ENOTSUP;
        return -1;
    }
    coded_width = ((uint32_t)input->picture_width_in_mbs_minus1 + 1u) * 16u;
    coded_height =
        ((uint32_t)input->picture_height_in_mbs_minus1 + 1u) * 16u;
    if (input->visible_width > coded_width ||
        input->visible_height > coded_height ||
        ((coded_width - input->visible_width) % 2u) != 0 ||
        ((coded_height - input->visible_height) % 2u) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int advc_h264_build_codec_config(
    const struct advc_h264_parameter_input *input, uint32_t pps_id,
    struct advc_h264_codec_config *config) {
    struct bit_writer sps;
    struct bit_writer pps;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t crop_right;
    uint32_t crop_bottom;
    uint8_t constraint_flags;

    if (config == NULL || pps_id > 255u || validate_parameters(input) < 0)
        return -1;
    memset(config, 0, sizeof(*config));
    memset(&sps, 0, sizeof(sps));
    memset(&pps, 0, sizeof(pps));

    coded_width = ((uint32_t)input->picture_width_in_mbs_minus1 + 1u) * 16u;
    coded_height =
        ((uint32_t)input->picture_height_in_mbs_minus1 + 1u) * 16u;
    crop_right = (coded_width - input->visible_width) / 2u;
    crop_bottom = (coded_height - input->visible_height) / 2u;
    constraint_flags = input->profile_idc == 66u ? 0xc0u : 0u;

    if (put_bits(&sps, input->profile_idc, 8) < 0 ||
        put_bits(&sps, constraint_flags, 8) < 0 ||
        put_bits(&sps, 51u, 8) < 0 || put_ue(&sps, 0) < 0 ||
        put_ue(&sps, input->log2_max_frame_num_minus4) < 0 ||
        put_ue(&sps, input->pic_order_cnt_type) < 0)
        return -1;
    if (input->pic_order_cnt_type == 0 &&
        put_ue(&sps, input->log2_max_pic_order_cnt_lsb_minus4) < 0)
        return -1;
    if (put_ue(&sps, input->num_ref_frames) < 0 ||
        put_bit(&sps, input->gaps_in_frame_num_value_allowed_flag) < 0 ||
        put_ue(&sps, input->picture_width_in_mbs_minus1) < 0 ||
        put_ue(&sps, input->picture_height_in_mbs_minus1) < 0 ||
        put_bit(&sps, input->frame_mbs_only_flag) < 0 ||
        put_bit(&sps, input->direct_8x8_inference_flag) < 0 ||
        put_bit(&sps, crop_right != 0 || crop_bottom != 0) < 0)
        return -1;
    if ((crop_right != 0 || crop_bottom != 0) &&
        (put_ue(&sps, 0) < 0 || put_ue(&sps, crop_right) < 0 ||
         put_ue(&sps, 0) < 0 || put_ue(&sps, crop_bottom) < 0))
        return -1;
    if (put_bit(&sps, input->vui_bitstream_restriction_flag) < 0)
        return -1; /* vui_parameters_present_flag */
    if (input->vui_bitstream_restriction_flag != 0) {
        /*
         * VA-API exposes decoded-picture parameters, not the original SPS.
         * The optional validation gate therefore emits only the bounded VUI
         * bitstream-restriction subset needed to describe decoder reordering;
         * aspect, colour, timing and HRD data are deliberately not guessed.
         */
        if (put_bit(&sps, 0) < 0 || /* aspect_ratio_info_present_flag */
            put_bit(&sps, 0) < 0 || /* overscan_info_present_flag */
            put_bit(&sps, 0) < 0 || /* video_signal_type_present_flag */
            put_bit(&sps, 0) < 0 || /* chroma_loc_info_present_flag */
            put_bit(&sps, 0) < 0 || /* timing_info_present_flag */
            put_bit(&sps, 0) < 0 || /* nal_hrd_parameters_present_flag */
            put_bit(&sps, 0) < 0 || /* vcl_hrd_parameters_present_flag */
            put_bit(&sps, 0) < 0 || /* pic_struct_present_flag */
            put_bit(&sps, 1) < 0 || /* bitstream_restriction_flag */
            put_bit(&sps, 1) < 0 || /* motion_vectors_over_pic_boundaries */
            put_ue(&sps, 2) < 0 ||  /* max_bytes_per_pic_denom */
            put_ue(&sps, 1) < 0 ||  /* max_bits_per_mb_denom */
            put_ue(&sps, 16) < 0 || /* log2_max_mv_length_horizontal */
            put_ue(&sps, 16) < 0 || /* log2_max_mv_length_vertical */
            put_ue(&sps, input->max_num_reorder_frames) < 0 ||
            put_ue(&sps, input->max_dec_frame_buffering) < 0)
            return -1;
    }
    if (finish_rbsp(&sps) < 0) return -1;

    if (put_ue(&pps, pps_id) < 0 || put_ue(&pps, 0) < 0 ||
        put_bit(&pps, input->entropy_coding_mode_flag) < 0 ||
        put_bit(&pps,
                input->bottom_field_pic_order_in_frame_present_flag) < 0 ||
        put_ue(&pps, 0) < 0 ||
        put_ue(&pps, input->num_ref_idx_l0_default_active_minus1) < 0 ||
        put_ue(&pps, input->num_ref_idx_l1_default_active_minus1) < 0 ||
        put_bit(&pps, input->weighted_pred_flag) < 0 ||
        put_bits(&pps, input->weighted_bipred_idc, 2) < 0 ||
        put_se(&pps, input->pic_init_qp_minus26) < 0 ||
        put_se(&pps, input->pic_init_qs_minus26) < 0 ||
        put_se(&pps, input->chroma_qp_index_offset) < 0 ||
        put_bit(&pps, input->deblocking_filter_control_present_flag) < 0 ||
        put_bit(&pps, input->constrained_intra_pred_flag) < 0 ||
        put_bit(&pps, input->redundant_pic_cnt_present_flag) < 0 ||
        finish_rbsp(&pps) < 0)
        return -1;

    if (append_annexb_nal(config->data, sizeof(config->data), &config->size,
                          0x67u, &sps) < 0 ||
        append_annexb_nal(config->data, sizeof(config->data), &config->size,
                          0x68u, &pps) < 0)
        return -1;
    config->pps_id = pps_id;
    return 0;
}

int advc_h264_build_access_unit(const struct advc_h264_slice_input *slices,
                                size_t slice_count, uint8_t **data,
                                size_t *size, uint32_t *pps_id, int *is_idr) {
    uint8_t *output;
    size_t output_size = 0;
    size_t i;
    uint32_t common_pps = 0;
    int any_idr = 0;

    if (slices == NULL || data == NULL || size == NULL || pps_id == NULL ||
        is_idr == NULL || slice_count == 0 ||
        slice_count > ADVC_H264_MAX_SLICES) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < slice_count; ++i) {
        size_t prefix;
        uint32_t parsed_first_mb;
        uint32_t parsed_slice_type;
        uint32_t parsed_pps;
        int slice_idr;

        if (advc_h264_parse_slice_identity(
                slices[i].data, slices[i].size, &parsed_first_mb,
                &parsed_slice_type, &parsed_pps, &slice_idr) < 0 ||
            parsed_first_mb != slices[i].expected_first_mb_in_slice ||
            (parsed_slice_type % 5u) !=
                ((uint32_t)slices[i].expected_slice_type % 5u)) {
            if (errno == 0) errno = EPROTO;
            return -1;
        }
        if (i == 0)
            common_pps = parsed_pps;
        else if (parsed_pps != common_pps) {
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
        any_idr |= slice_idr;
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
    *is_idr = any_idr;
    return 0;
}

int advc_h264_prepend_codec_config(
    const struct advc_h264_codec_config *config, uint8_t **data,
    size_t *size) {
    uint8_t *joined;
    if (config == NULL || data == NULL || size == NULL || *data == NULL ||
        config->size == 0 || config->size > ADVC_H264_MAX_CONFIG_BYTES ||
        *size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (config->size > ADVC_MAX_INPUT_BYTES ||
        *size > ADVC_MAX_INPUT_BYTES - config->size) {
        errno = EOVERFLOW;
        return -1;
    }
    joined = malloc(config->size + *size);
    if (joined == NULL) return -1;
    memcpy(joined, config->data, config->size);
    memcpy(joined + config->size, *data, *size);
    free(*data);
    *data = joined;
    *size += config->size;
    return 0;
}
