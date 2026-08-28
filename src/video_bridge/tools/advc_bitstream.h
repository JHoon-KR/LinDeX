#ifndef ADVC_BITSTREAM_H
#define ADVC_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

enum advc_bitstream_format {
    ADVC_BITSTREAM_UNKNOWN = 0,
    ADVC_BITSTREAM_ANNEX_B = 1,
    ADVC_BITSTREAM_LENGTH_PREFIXED = 2,
    ADVC_BITSTREAM_CODEC_CONFIG_RECORD = 3,
};

struct advc_bitstream_stats {
    enum advc_bitstream_format format;
    uint32_t nal_units;
    uint32_t parameter_sets;
    /* AVC: bit 0 SPS, bit 1 PPS. HEVC: bit 0 VPS, bit 1 SPS, bit 2 PPS. */
    uint32_t parameter_set_mask;
    uint32_t vcl_units;
    uint32_t key_vcl_units;
};

/*
 * Validates exactly one nonempty AVC or HEVC output packet. The accepted forms are
 * Annex-B, four-byte big-endian length-prefixed NAL units, AVCDecoderConfiguration-
 * Record, and HEVCDecoderConfigurationRecord. Returns zero on structural success.
 */
int advc_bitstream_inspect(const char *mime, const uint8_t *data, size_t size,
                           struct advc_bitstream_stats *stats);

const char *advc_bitstream_format_name(enum advc_bitstream_format format);

#endif
