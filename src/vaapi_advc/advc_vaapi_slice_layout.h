#ifndef ADVC_VAAPI_SLICE_LAYOUT_H
#define ADVC_VAAPI_SLICE_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rebase offsets which are relative to one VASliceDataBuffer onto an
 * aggregate access-unit buffer.  Validation is transactional: no offset is
 * changed unless every span is contained in the local data buffer and the
 * aggregate size is representable by libva's 32-bit slice_data_offset.
 */
int advc_vaapi_rebase_slice_offsets(uint32_t *offsets,
                                    const uint32_t *sizes,
                                    size_t span_count,
                                    size_t aggregate_size,
                                    size_t local_data_size,
                                    size_t maximum_size);

#ifdef __cplusplus
}
#endif

#endif
