#include "advc_annexb.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int find_start_code(const uint8_t *data, size_t size, size_t from,
                           size_t *position, size_t *length) {
    for (size_t i = from; i < size; ++i) {
        if (i + 2 < size && data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *position = i;
                *length = 3;
                return 0;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                *position = i;
                *length = 4;
                return 0;
            }
        }
    }
    return -1;
}

static int bytes_are_zero(const uint8_t *data, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        if (data[i] != 0) return 0;
    }
    return 1;
}

int advc_avc_annexb_split(const uint8_t *data, size_t size,
                          struct advc_avc_annexb_parts *parts) {
    size_t start;
    size_t start_length;
    size_t frame_offset = SIZE_MAX;
    int saw_sps = 0;
    int saw_pps = 0;
    int saw_idr = 0;
    if (data == NULL || parts == NULL || size < 4 ||
        find_start_code(data, size, 0, &start, &start_length) < 0 ||
        !bytes_are_zero(data, 0, start)) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        size_t header = start + start_length;
        size_t next;
        size_t next_length;
        size_t end;
        uint8_t nal_type;
        int have_next;
        if (header >= size) goto malformed;
        have_next = find_start_code(data, size, header + 1, &next, &next_length) == 0;
        end = have_next ? next : size;
        if (end <= header + 1 || (data[header] & 0x80u) != 0) goto malformed;
        nal_type = data[header] & 0x1fu;
        if (nal_type == 0 || nal_type > 23) goto malformed;
        if (frame_offset == SIZE_MAX) {
            if (nal_type == 7) {
                saw_sps = 1;
            } else if (nal_type == 8) {
                saw_pps = 1;
            } else if (nal_type == 5) {
                if (!saw_sps || !saw_pps) goto malformed;
                frame_offset = start;
                saw_idr = 1;
            } else if (nal_type == 6 || nal_type == 9 || nal_type == 10 ||
                       nal_type == 11 || nal_type == 12) {
                /*
                 * Encoders commonly place SEI or an access-unit delimiter
                 * after PPS and before the first IDR.  Keep SPS/PPS alone in
                 * CODEC_CONFIG and submit the leading non-VCL NAL with the
                 * access unit.
                 */
                if (!saw_sps || !saw_pps) goto malformed;
                frame_offset = start;
            } else {
                /* Keep the codec-config prefix unambiguous and data-only. */
                goto malformed;
            }
        } else if (nal_type >= 1 && nal_type <= 4) {
            /* The bounded smoke sample must remain one IDR access unit. */
            goto malformed;
        } else if (nal_type == 5) {
            saw_idr = 1;
        } else if (nal_type != 6 && nal_type != 9 && nal_type != 10 &&
                   nal_type != 11 && nal_type != 12) {
            goto malformed;
        }
        if (!have_next) break;
        start = next;
        start_length = next_length;
    }
    if (frame_offset == SIZE_MAX || !saw_idr || frame_offset == 0 || frame_offset >= size)
        goto malformed;
    memset(parts, 0, sizeof(*parts));
    parts->config_offset = 0;
    parts->config_size = frame_offset;
    parts->frame_offset = frame_offset;
    parts->frame_size = size - frame_offset;
    return 0;

malformed:
    errno = EINVAL;
    return -1;
}

int advc_avc_annexb_split_single_slice_stream(
    const uint8_t *data, size_t size, struct advc_avc_annexb_stream *stream) {
    size_t start;
    size_t start_length;
    size_t frame_start = SIZE_MAX;
    int saw_sps = 0;
    int saw_pps = 0;
    int saw_vcl = 0;
    int first_vcl = 1;
    int current_key = 0;

    if (data == NULL || stream == NULL || size < 4 ||
        find_start_code(data, size, 0, &start, &start_length) < 0 ||
        !bytes_are_zero(data, 0, start))
        goto malformed_stream;
    memset(stream, 0, sizeof(*stream));
    for (;;) {
        size_t header = start + start_length;
        size_t next;
        size_t next_length;
        size_t end;
        uint8_t nal_type;
        int have_next;
        if (header >= size) goto malformed_stream;
        have_next = find_start_code(data, size, header + 1, &next, &next_length) == 0;
        end = have_next ? next : size;
        if (end <= header + 1 || (data[header] & 0x80u) != 0) goto malformed_stream;
        nal_type = data[header] & 0x1fu;
        if (nal_type == 0 || nal_type > 23) goto malformed_stream;
        if (frame_start == SIZE_MAX) {
            if (nal_type == 7) {
                saw_sps = 1;
            } else if (nal_type == 8) {
                saw_pps = 1;
            } else {
                if (!saw_sps || !saw_pps) goto malformed_stream;
                frame_start = start;
                stream->config_offset = 0;
                stream->config_size = start;
            }
        }
        if (nal_type >= 1 && nal_type <= 5) {
            if (first_vcl && nal_type != 5) goto malformed_stream;
            if (saw_vcl) {
                if (stream->frame_count >= ADVC_AVC_MAX_SMOKE_FRAMES)
                    goto malformed_stream;
                stream->frames[stream->frame_count].offset = frame_start;
                stream->frames[stream->frame_count].size = start - frame_start;
                stream->frames[stream->frame_count].key_frame = current_key;
                ++stream->frame_count;
                frame_start = start;
                current_key = 0;
            }
            if (nal_type == 5) current_key = 1;
            saw_vcl = 1;
            first_vcl = 0;
        }
        if (!have_next) break;
        start = next;
        start_length = next_length;
    }
    if (!saw_vcl || frame_start == SIZE_MAX || frame_start >= size ||
        stream->frame_count >= ADVC_AVC_MAX_SMOKE_FRAMES)
        goto malformed_stream;
    stream->frames[stream->frame_count].offset = frame_start;
    stream->frames[stream->frame_count].size = size - frame_start;
    stream->frames[stream->frame_count].key_frame = current_key;
    ++stream->frame_count;
    return 0;

malformed_stream:
    errno = EINVAL;
    return -1;
}
