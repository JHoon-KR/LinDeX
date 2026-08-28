#ifndef ADVC_VAAPI_DECODE_AV1_H
#define ADVC_VAAPI_DECODE_AV1_H

#include <stddef.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_dec_av1.h>

#ifdef __cplusplus
extern "C" {
#endif

enum advc_av1_input_format {
    ADVC_AV1_INPUT_RAW_OBU = 1,
    ADVC_AV1_INPUT_ANNEX_B = 2,
};

struct advc_av1_access_unit_info {
    enum advc_av1_input_format input_format;
    uint8_t frame_type;
    uint8_t key_frame;
    uint8_t contains_sequence_header;
    uint8_t reserved;
    uint32_t obu_count;
};

/*
 * Validate and translate one complete VA-API AV1 Main, 8-bit, 4:2:0 frame.
 *
 * Low-overhead input (one or more size-delimited OBUs) is copied byte for
 * byte.  One Annex-B temporal unit containing exactly one frame unit is
 * losslessly unwrapped into low-overhead OBUs; an OBU size field is inserted
 * where Annex-B omitted it.  MediaCodec consumes the returned allocation as
 * one access unit.
 *
 * This function never reconstructs missing sequence/frame/tile-group syntax
 * from VA picture parameters.  All tile parameters must describe complete
 * (VA_SLICE_DATA_FLAG_ALL), non-overlapping ranges inside an original FRAME or
 * TILE_GROUP OBU payload.  Header-stripped tile buffers and fragmented VA
 * submissions fail closed with ENOTSUP.
 */
int advc_av1_build_access_unit(
    const VADecPictureParameterBufferAV1 *picture,
    const VASliceParameterBufferAV1 *tiles, size_t tile_count,
    const uint8_t *slice_data, size_t slice_data_bytes,
    uint32_t expected_width, uint32_t expected_height,
    uint8_t **access_unit, size_t *access_unit_size,
    struct advc_av1_access_unit_info *info);

#ifdef __cplusplus
}
#endif

#endif
