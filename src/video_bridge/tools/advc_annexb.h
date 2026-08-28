#ifndef ADVC_ANNEXB_H
#define ADVC_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

struct advc_avc_annexb_parts {
    size_t config_offset;
    size_t config_size;
    size_t frame_offset;
    size_t frame_size;
};

#define ADVC_AVC_MAX_SMOKE_FRAMES 256u

struct advc_avc_annexb_frame {
    size_t offset;
    size_t size;
    int key_frame;
};

struct advc_avc_annexb_stream {
    size_t config_offset;
    size_t config_size;
    size_t frame_count;
    struct advc_avc_annexb_frame frames[ADVC_AVC_MAX_SMOKE_FRAMES];
};

/*
 * Splits a bounded AVC Annex-B sample containing an SPS/PPS-only prefix and one
 * IDR access unit. Returns zero on success or -1 with errno=EINVAL when the
 * sample is malformed, lacks SPS/PPS, lacks VCL, or starts with non-IDR VCL.
 */
int advc_avc_annexb_split(const uint8_t *data, size_t size,
                          struct advc_avc_annexb_parts *parts);

/* Smoke-only splitter for streams known to contain one VCL NAL per picture. */
int advc_avc_annexb_split_single_slice_stream(
    const uint8_t *data, size_t size, struct advc_avc_annexb_stream *stream);

#endif
