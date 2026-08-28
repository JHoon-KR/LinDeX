#include "advc_vaapi_decode_vp9.h"

#include "advc/protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ADVC_VP9_FRAME_MARKER 2u

int advc_vp9_parse_frame_prefix(const uint8_t *data, size_t size,
                                struct advc_vp9_frame_info *info) {
    uint8_t first;
    uint8_t profile;
    uint8_t show_existing_frame;

    if (data == NULL || info == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    first = data[0];
    if (((first >> 6) & 3u) != ADVC_VP9_FRAME_MARKER) {
        errno = EPROTO;
        return -1;
    }
    profile = (uint8_t)(((first >> 5) & 1u) |
                        (((first >> 4) & 1u) << 1));
    /* Profile 3 carries one more reserved bit; it is outside this subset. */
    if (profile != 0) {
        errno = ENOTSUP;
        return -1;
    }
    show_existing_frame = (uint8_t)((first >> 3) & 1u);
    if (show_existing_frame != 0) {
        errno = ENOTSUP;
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->profile = profile;
    info->key_frame = (uint8_t)(((first >> 2) & 1u) == 0);
    info->show_frame = (uint8_t)((first >> 1) & 1u);
    info->error_resilient_mode = (uint8_t)(first & 1u);
    if (info->key_frame) {
        if (size < 4 || data[1] != 0x49 || data[2] != 0x83 ||
            data[3] != 0x42) {
            errno = EPROTO;
            return -1;
        }
    }
    return 0;
}

int advc_vp9_build_access_unit(
    const VADecPictureParameterBufferVP9 *picture,
    const VASliceParameterBufferVP9 *slice, const uint8_t *slice_data,
    size_t slice_data_bytes, uint32_t expected_width,
    uint32_t expected_height, uint8_t **access_unit,
    size_t *access_unit_size, int *key_frame) {
    struct advc_vp9_frame_info parsed;
    uint8_t *copy;

    if (picture == NULL || slice == NULL || slice_data == NULL ||
        access_unit == NULL || access_unit_size == NULL || key_frame == NULL ||
        expected_width == 0 || expected_height == 0 ||
        expected_width > UINT16_MAX || expected_height > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    *access_unit = NULL;
    *access_unit_size = 0;
    *key_frame = 0;

    /* libva's VP9 VLD API is 8-bit 4:2:0 only for Profile 0 here. */
    if (picture->profile != 0 || picture->bit_depth != 8 ||
        picture->pic_fields.bits.subsampling_x != 1 ||
        picture->pic_fields.bits.subsampling_y != 1) {
        errno = ENOTSUP;
        return -1;
    }
    if (picture->frame_width != expected_width ||
        picture->frame_height != expected_height) {
        errno = EINVAL;
        return -1;
    }

    /*
     * FFmpeg supplies one complete VP9 frame with offset zero and ALL. Do not
     * skip frame_header_length_in_bytes: MediaCodec needs both the
     * uncompressed and compressed headers. Fragment assembly is not guessed.
     */
    if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
        slice->slice_data_offset != 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (slice->slice_data_size == 0 ||
        slice->slice_data_size != slice_data_bytes) {
        errno = EINVAL;
        return -1;
    }
    if (slice_data_bytes > ADVC_MAX_INPUT_BYTES) {
        errno = E2BIG;
        return -1;
    }
    if (advc_vp9_parse_frame_prefix(slice_data, slice_data_bytes, &parsed) < 0)
        return -1;
    if (parsed.profile != picture->profile ||
        parsed.key_frame != (picture->pic_fields.bits.frame_type == 0) ||
        parsed.show_frame != picture->pic_fields.bits.show_frame ||
        parsed.error_resilient_mode !=
            picture->pic_fields.bits.error_resilient_mode) {
        errno = EPROTO;
        return -1;
    }

    copy = malloc(slice_data_bytes);
    if (copy == NULL) return -1;
    memcpy(copy, slice_data, slice_data_bytes);
    *access_unit = copy;
    *access_unit_size = slice_data_bytes;
    *key_frame = parsed.key_frame;
    return 0;
}
