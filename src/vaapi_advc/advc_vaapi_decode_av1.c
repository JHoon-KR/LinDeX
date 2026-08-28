#include "advc_vaapi_decode_av1.h"

#include "advc/protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ADVC_AV1_MAX_TILES 512u
#define ADVC_AV1_MAX_OBUS 1024u

enum advc_av1_obu_type {
    ADVC_AV1_OBU_SEQUENCE_HEADER = 1,
    ADVC_AV1_OBU_TEMPORAL_DELIMITER = 2,
    ADVC_AV1_OBU_FRAME_HEADER = 3,
    ADVC_AV1_OBU_TILE_GROUP = 4,
    ADVC_AV1_OBU_METADATA = 5,
    ADVC_AV1_OBU_FRAME = 6,
    ADVC_AV1_OBU_REDUNDANT_FRAME_HEADER = 7,
    ADVC_AV1_OBU_TILE_LIST = 8,
    ADVC_AV1_OBU_PADDING = 15,
};

struct advc_av1_payload_span {
    size_t begin;
    size_t end;
};

struct advc_av1_parse_state {
    const VADecPictureParameterBufferAV1 *picture;
    uint32_t obu_count;
    uint32_t tile_group_count;
    int sequence_seen;
    int reduced_still_picture_header_known;
    int reduced_still_picture_header;
    int frame_header_seen;
    int frame_obu_seen;
    int complete_frame;
    struct advc_av1_payload_span payloads[ADVC_AV1_MAX_TILES];
    size_t payload_count;
};

struct advc_av1_obu {
    uint8_t header;
    uint8_t type;
    uint8_t extension;
    size_t header_size;
    size_t payload_offset;
    size_t payload_size;
};

static int add_size(size_t left, size_t right, size_t *result) {
    if (result == NULL || left > SIZE_MAX - right) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = left + right;
    return 0;
}

static int read_leb128(const uint8_t *data, size_t size, size_t *consumed,
                       size_t *value) {
    uint64_t decoded = 0;
    unsigned int i;
    if (data == NULL || consumed == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < 8u; ++i) {
        uint8_t byte;
        if ((size_t)i >= size) {
            errno = EPROTO;
            return -1;
        }
        byte = data[i];
        decoded |= (uint64_t)(byte & 0x7fu) << (7u * i);
        if ((byte & 0x80u) == 0) {
            if (decoded > SIZE_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            *consumed = (size_t)i + 1u;
            *value = (size_t)decoded;
            return 0;
        }
    }
    errno = EPROTO;
    return -1;
}

static size_t leb128_size(size_t value) {
    size_t bytes = 1;
    while (value >= 0x80u) {
        value >>= 7;
        ++bytes;
    }
    return bytes;
}

static size_t write_leb128(uint8_t *output, size_t value) {
    size_t written = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        if (value != 0) byte |= 0x80u;
        output[written++] = byte;
    } while (value != 0);
    return written;
}

static int parse_obu(const uint8_t *data, size_t size, size_t absolute_offset,
                     int annex_b, struct advc_av1_obu *obu) {
    uint8_t header;
    size_t cursor;
    size_t field_size;
    size_t payload_size;
    if (data == NULL || obu == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    header = data[0];
    if ((header & 0x81u) != 0) {
        errno = EPROTO;
        return -1;
    }
    memset(obu, 0, sizeof(*obu));
    obu->header = header;
    obu->type = (uint8_t)((header >> 3) & 0x0fu);
    obu->extension = (uint8_t)((header >> 2) & 1u);
    cursor = 1;
    if (obu->extension != 0) {
        uint8_t extension;
        if (cursor >= size) {
            errno = EPROTO;
            return -1;
        }
        extension = data[cursor++];
        /* This bridge exposes one non-scalable MediaCodec output stream. */
        if (extension != 0) {
            errno = ENOTSUP;
            return -1;
        }
    }
    obu->header_size = cursor;
    if ((header & 0x02u) != 0) {
        if (read_leb128(data + cursor, size - cursor, &field_size,
                        &payload_size) < 0)
            return -1;
        if (field_size > size - cursor ||
            payload_size != size - cursor - field_size) {
            errno = EPROTO;
            return -1;
        }
        cursor += field_size;
    } else {
        if (!annex_b) {
            errno = ENOTSUP;
            return -1;
        }
        payload_size = size - cursor;
    }
    if (absolute_offset > SIZE_MAX - cursor) {
        errno = EOVERFLOW;
        return -1;
    }
    obu->payload_offset = absolute_offset + cursor;
    obu->payload_size = payload_size;
    return 0;
}

static int read_payload_bit(const uint8_t *payload, size_t payload_size,
                            size_t *bit_offset, unsigned int *value) {
    size_t byte_offset;
    unsigned int shift;
    if (payload == NULL || bit_offset == NULL || value == NULL ||
        *bit_offset >= payload_size * CHAR_BIT) {
        errno = EPROTO;
        return -1;
    }
    byte_offset = *bit_offset / CHAR_BIT;
    shift = (unsigned int)(CHAR_BIT - 1u - (*bit_offset % CHAR_BIT));
    *value = (payload[byte_offset] >> shift) & 1u;
    ++*bit_offset;
    return 0;
}

static int read_payload_bits(const uint8_t *payload, size_t payload_size,
                             size_t *bit_offset, unsigned int count,
                             unsigned int *value) {
    unsigned int result = 0;
    unsigned int bit;
    unsigned int i;
    if (count > sizeof(result) * CHAR_BIT || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (read_payload_bit(payload, payload_size, bit_offset, &bit) < 0)
            return -1;
        result = (result << 1) | bit;
    }
    *value = result;
    return 0;
}

static int check_sequence_prefix(struct advc_av1_parse_state *state,
                                 const uint8_t *payload,
                                 size_t payload_size) {
    size_t bit_offset = 0;
    unsigned int profile;
    unsigned int still_picture;
    unsigned int reduced;
    if (read_payload_bits(payload, payload_size, &bit_offset, 3, &profile) < 0 ||
        read_payload_bit(payload, payload_size, &bit_offset, &still_picture) < 0 ||
        read_payload_bit(payload, payload_size, &bit_offset, &reduced) < 0)
        return -1;
    if (profile != state->picture->profile || profile != 0u) {
        errno = ENOTSUP;
        return -1;
    }
    if (still_picture != state->picture->seq_info_fields.fields.still_picture ||
        (reduced != 0 && still_picture == 0)) {
        errno = EPROTO;
        return -1;
    }
    state->reduced_still_picture_header_known = 1;
    state->reduced_still_picture_header = reduced != 0;
    return 0;
}

static int check_frame_prefix(struct advc_av1_parse_state *state,
                              const uint8_t *payload, size_t payload_size) {
    size_t bit_offset = 0;
    unsigned int show_existing;
    unsigned int frame_type;
    unsigned int show_frame;
    if (!state->reduced_still_picture_header_known) {
        if (state->picture->seq_info_fields.fields.still_picture != 0) {
            errno = ENOTSUP;
            return -1;
        }
        state->reduced_still_picture_header_known = 1;
        state->reduced_still_picture_header = 0;
    }
    if (state->reduced_still_picture_header) {
        frame_type = 0;
        show_frame = 1;
    } else {
        if (read_payload_bit(payload, payload_size, &bit_offset,
                             &show_existing) < 0)
            return -1;
        if (show_existing != 0) {
            errno = ENOTSUP;
            return -1;
        }
        if (read_payload_bits(payload, payload_size, &bit_offset, 2,
                              &frame_type) < 0 ||
            read_payload_bit(payload, payload_size, &bit_offset,
                             &show_frame) < 0)
            return -1;
    }
    if (frame_type != state->picture->pic_info_fields.bits.frame_type ||
        show_frame != state->picture->pic_info_fields.bits.show_frame) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int observe_obu(struct advc_av1_parse_state *state,
                       const struct advc_av1_obu *obu,
                       const uint8_t *payload) {
    if (state->obu_count >= ADVC_AV1_MAX_OBUS) {
        errno = E2BIG;
        return -1;
    }
    ++state->obu_count;
    switch (obu->type) {
    case ADVC_AV1_OBU_SEQUENCE_HEADER:
        if (state->sequence_seen || state->frame_header_seen ||
            state->frame_obu_seen || check_sequence_prefix(
                state, payload, obu->payload_size) < 0) {
            if (errno == 0) errno = ENOTSUP;
            return -1;
        }
        state->sequence_seen = 1;
        return 0;
    case ADVC_AV1_OBU_TEMPORAL_DELIMITER:
        if (obu->payload_size != 0 || state->frame_header_seen ||
            state->frame_obu_seen || state->tile_group_count != 0) {
            errno = EPROTO;
            return -1;
        }
        return 0;
    case ADVC_AV1_OBU_FRAME_HEADER:
        if (state->frame_header_seen || state->frame_obu_seen ||
            state->tile_group_count != 0 ||
            check_frame_prefix(state, payload, obu->payload_size) < 0)
            return -1;
        state->frame_header_seen = 1;
        return 0;
    case ADVC_AV1_OBU_TILE_GROUP:
        if (!state->frame_header_seen || state->frame_obu_seen ||
            state->payload_count >= ADVC_AV1_MAX_TILES) {
            errno = ENOTSUP;
            return -1;
        }
        state->payloads[state->payload_count].begin = obu->payload_offset;
        state->payloads[state->payload_count].end =
            obu->payload_offset + obu->payload_size;
        ++state->payload_count;
        ++state->tile_group_count;
        state->complete_frame = 1;
        return 0;
    case ADVC_AV1_OBU_FRAME:
        if (state->frame_header_seen || state->frame_obu_seen ||
            state->tile_group_count != 0 ||
            check_frame_prefix(state, payload, obu->payload_size) < 0)
            return -1;
        state->frame_obu_seen = 1;
        state->complete_frame = 1;
        state->payloads[0].begin = obu->payload_offset;
        state->payloads[0].end = obu->payload_offset + obu->payload_size;
        state->payload_count = 1;
        return 0;
    case ADVC_AV1_OBU_METADATA:
        if (state->frame_header_seen || state->frame_obu_seen ||
            state->tile_group_count != 0) {
            errno = ENOTSUP;
            return -1;
        }
        return 0;
    case ADVC_AV1_OBU_PADDING:
        return 0;
    case ADVC_AV1_OBU_REDUNDANT_FRAME_HEADER:
    case ADVC_AV1_OBU_TILE_LIST:
        errno = ENOTSUP;
        return -1;
    default:
        errno = ENOTSUP;
        return -1;
    }
}

static int finish_parse(const struct advc_av1_parse_state *state) {
    if (!state->complete_frame || state->payload_count == 0 ||
        (state->frame_header_seen && state->tile_group_count == 0)) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int parse_raw_obus(const uint8_t *data, size_t size,
                          const VADecPictureParameterBufferAV1 *picture,
                          struct advc_av1_parse_state *state) {
    size_t offset = 0;
    memset(state, 0, sizeof(*state));
    state->picture = picture;
    while (offset < size) {
        struct advc_av1_obu obu;
        size_t header_size;
        size_t payload_size;
        size_t field_size;
        size_t obu_size;
        uint8_t header = data[offset];
        header_size = 1u + (((header >> 2) & 1u) != 0);
        if (header_size >= size - offset || (header & 0x02u) == 0 ||
            read_leb128(data + offset + header_size,
                        size - offset - header_size, &field_size,
                        &payload_size) < 0 ||
            add_size(header_size, field_size, &obu_size) < 0 ||
            add_size(obu_size, payload_size, &obu_size) < 0 ||
            obu_size > size - offset ||
            parse_obu(data + offset, obu_size, offset, 0, &obu) < 0 ||
            observe_obu(state, &obu, data + obu.payload_offset) < 0)
            return -1;
        offset += obu_size;
    }
    return finish_parse(state);
}

static int parse_annex_b(const uint8_t *data, size_t size,
                         const VADecPictureParameterBufferAV1 *picture,
                         struct advc_av1_parse_state *state,
                         uint8_t *output, size_t output_capacity,
                         size_t *normalized_size) {
    size_t tu_field;
    size_t tu_size;
    size_t fu_field;
    size_t fu_size;
    size_t cursor;
    size_t fu_end;
    size_t out_size = 0;
    memset(state, 0, sizeof(*state));
    state->picture = picture;
    if (read_leb128(data, size, &tu_field, &tu_size) < 0 ||
        tu_size != size - tu_field) {
        errno = EPROTO;
        return -1;
    }
    cursor = tu_field;
    if (read_leb128(data + cursor, size - cursor, &fu_field, &fu_size) < 0 ||
        fu_field > size - cursor || fu_size != size - cursor - fu_field) {
        errno = EPROTO;
        return -1;
    }
    cursor += fu_field;
    fu_end = cursor + fu_size;
    while (cursor < fu_end) {
        struct advc_av1_obu obu;
        size_t length_field;
        size_t obu_size;
        size_t normalized_obu_size;
        size_t size_field;
        size_t payload_relative;
        if (read_leb128(data + cursor, fu_end - cursor, &length_field,
                        &obu_size) < 0 || length_field > fu_end - cursor ||
            obu_size == 0 || obu_size > fu_end - cursor - length_field)
            return -1;
        cursor += length_field;
        if (parse_obu(data + cursor, obu_size, cursor, 1, &obu) < 0 ||
            observe_obu(state, &obu, data + obu.payload_offset) < 0)
            return -1;
        size_field = leb128_size(obu.payload_size);
        if (add_size(obu.header_size, size_field, &normalized_obu_size) < 0 ||
            add_size(normalized_obu_size, obu.payload_size,
                     &normalized_obu_size) < 0 ||
            out_size > ADVC_MAX_INPUT_BYTES - normalized_obu_size) {
            errno = E2BIG;
            return -1;
        }
        if (output != NULL) {
            if (out_size > output_capacity ||
                normalized_obu_size > output_capacity - out_size) {
                errno = EOVERFLOW;
                return -1;
            }
            output[out_size] = obu.header | 0x02u;
            if (obu.header_size == 2u)
                output[out_size + 1u] = data[cursor + 1u];
            payload_relative = obu.payload_offset - cursor;
            (void)write_leb128(output + out_size + obu.header_size,
                               obu.payload_size);
            memcpy(output + out_size + obu.header_size + size_field,
                   data + cursor + payload_relative, obu.payload_size);
        }
        out_size += normalized_obu_size;
        cursor += obu_size;
    }
    if (cursor != fu_end || finish_parse(state) < 0) return -1;
    *normalized_size = out_size;
    return 0;
}

static int validate_picture(const VADecPictureParameterBufferAV1 *picture,
                            uint32_t expected_width,
                            uint32_t expected_height) {
    if (picture == NULL || expected_width == 0 || expected_height == 0 ||
        expected_width > UINT16_MAX || expected_height > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (picture->profile != 0 || picture->bit_depth_idx != 0 ||
        picture->seq_info_fields.fields.mono_chrome != 0 ||
        picture->seq_info_fields.fields.subsampling_x != 1 ||
        picture->seq_info_fields.fields.subsampling_y != 1 ||
        picture->pic_info_fields.bits.large_scale_tile != 0 ||
        picture->anchor_frames_num != 0) {
        errno = ENOTSUP;
        return -1;
    }
    if ((uint32_t)picture->frame_width_minus1 + 1u != expected_width ||
        (uint32_t)picture->frame_height_minus1 + 1u != expected_height) {
        errno = EINVAL;
        return -1;
    }
    if (picture->tile_cols == 0 || picture->tile_rows == 0 ||
        (size_t)picture->tile_cols * picture->tile_rows > ADVC_AV1_MAX_TILES) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int validate_tiles(const VASliceParameterBufferAV1 *tiles,
                          size_t tile_count, size_t data_size,
                          const VADecPictureParameterBufferAV1 *picture,
                          const struct advc_av1_parse_state *state) {
    uint8_t seen[ADVC_AV1_MAX_TILES];
    size_t expected_count = (size_t)picture->tile_cols * picture->tile_rows;
    size_t i;
    size_t j;
    if (tiles == NULL || tile_count != expected_count || tile_count == 0) {
        errno = ENOTSUP;
        return -1;
    }
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < tile_count; ++i) {
        size_t begin = tiles[i].slice_data_offset;
        size_t end;
        size_t index;
        int contained = 0;
        if (tiles[i].slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
            tiles[i].slice_data_size == 0 ||
            tiles[i].tile_row >= picture->tile_rows ||
            tiles[i].tile_column >= picture->tile_cols ||
            begin > data_size ||
            tiles[i].slice_data_size > data_size - begin) {
            errno = ENOTSUP;
            return -1;
        }
        end = begin + tiles[i].slice_data_size;
        index = (size_t)tiles[i].tile_row * picture->tile_cols +
                tiles[i].tile_column;
        if (seen[index]) {
            errno = EPROTO;
            return -1;
        }
        seen[index] = 1;
        for (j = 0; j < state->payload_count; ++j) {
            if (begin >= state->payloads[j].begin &&
                end <= state->payloads[j].end) {
                contained = 1;
                break;
            }
        }
        if (!contained) {
            errno = ENOTSUP;
            return -1;
        }
        for (j = 0; j < i; ++j) {
            size_t other_begin = tiles[j].slice_data_offset;
            size_t other_end = other_begin + tiles[j].slice_data_size;
            if (begin < other_end && other_begin < end) {
                errno = EPROTO;
                return -1;
            }
        }
    }
    return 0;
}

int advc_av1_build_access_unit(
    const VADecPictureParameterBufferAV1 *picture,
    const VASliceParameterBufferAV1 *tiles, size_t tile_count,
    const uint8_t *slice_data, size_t slice_data_bytes,
    uint32_t expected_width, uint32_t expected_height,
    uint8_t **access_unit, size_t *access_unit_size,
    struct advc_av1_access_unit_info *info) {
    struct advc_av1_parse_state raw_state;
    struct advc_av1_parse_state annex_state;
    int raw_ok;
    int raw_error;
    int annex_ok;
    int annex_error;
    size_t normalized_size = 0;
    uint8_t *output;

    if (slice_data == NULL || slice_data_bytes == 0 ||
        slice_data_bytes > ADVC_MAX_INPUT_BYTES || access_unit == NULL ||
        access_unit_size == NULL || info == NULL) {
        errno = slice_data_bytes > ADVC_MAX_INPUT_BYTES ? E2BIG : EINVAL;
        return -1;
    }
    *access_unit = NULL;
    *access_unit_size = 0;
    memset(info, 0, sizeof(*info));
    if (validate_picture(picture, expected_width, expected_height) < 0)
        return -1;

    errno = 0;
    raw_ok = parse_raw_obus(slice_data, slice_data_bytes, picture,
                            &raw_state) == 0;
    raw_error = errno;
    errno = 0;
    annex_ok = parse_annex_b(slice_data, slice_data_bytes, picture,
                             &annex_state, NULL, 0, &normalized_size) == 0;
    annex_error = errno;
    if (raw_ok && annex_ok) {
        errno = EPROTO;
        return -1;
    }
    if (!raw_ok && !annex_ok) {
        errno = raw_error == ENOTSUP || annex_error == ENOTSUP ? ENOTSUP :
                (raw_error != 0 ? raw_error : annex_error);
        return -1;
    }

    if (raw_ok) {
        if (validate_tiles(tiles, tile_count, slice_data_bytes, picture,
                           &raw_state) < 0)
            return -1;
        output = malloc(slice_data_bytes);
        if (output == NULL) return -1;
        memcpy(output, slice_data, slice_data_bytes);
        *access_unit_size = slice_data_bytes;
        info->input_format = ADVC_AV1_INPUT_RAW_OBU;
        info->contains_sequence_header = raw_state.sequence_seen != 0;
        info->obu_count = raw_state.obu_count;
    } else {
        if (validate_tiles(tiles, tile_count, slice_data_bytes, picture,
                           &annex_state) < 0)
            return -1;
        output = malloc(normalized_size);
        if (output == NULL) return -1;
        if (parse_annex_b(slice_data, slice_data_bytes, picture, &annex_state,
                          output, normalized_size, &normalized_size) < 0) {
            free(output);
            return -1;
        }
        *access_unit_size = normalized_size;
        info->input_format = ADVC_AV1_INPUT_ANNEX_B;
        info->contains_sequence_header = annex_state.sequence_seen != 0;
        info->obu_count = annex_state.obu_count;
    }
    info->frame_type =
        (uint8_t)picture->pic_info_fields.bits.frame_type;
    info->key_frame = info->frame_type == 0;
    *access_unit = output;
    return 0;
}
