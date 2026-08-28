#ifndef ADVC_VAAPI_DECODE_VP9_H
#define ADVC_VAAPI_DECODE_VP9_H

#include <stddef.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_dec_vp9.h>

#ifdef __cplusplus
extern "C" {
#endif

struct advc_vp9_frame_info {
    uint8_t profile;
    uint8_t key_frame;
    uint8_t show_frame;
    uint8_t error_resilient_mode;
};

/*
 * Parse only the fixed prefix of a VP9 uncompressed header. This deliberately
 * does not replace a VP9 parser: it supplies an independent consistency check
 * for the already parsed VA picture parameters before bytes cross the broker.
 */
int advc_vp9_parse_frame_prefix(const uint8_t *data, size_t size,
                                struct advc_vp9_frame_info *info);

/*
 * Build one MediaCodec input access unit from the fail-closed VA-API Profile 0
 * subset. The returned allocation contains the complete original VP9 frame;
 * no IVF/WebM framing and no reconstructed header is added.
 */
int advc_vp9_build_access_unit(
    const VADecPictureParameterBufferVP9 *picture,
    const VASliceParameterBufferVP9 *slice, const uint8_t *slice_data,
    size_t slice_data_bytes, uint32_t expected_width,
    uint32_t expected_height, uint8_t **access_unit,
    size_t *access_unit_size, int *key_frame);

#ifdef __cplusplus
}
#endif

#endif
