#ifndef ADVC_BYTE_RANGE_H
#define ADVC_BYTE_RANGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 and an overflow-safe subrange, or -1 for invalid signed metadata. */
int advc_checked_byte_range(const uint8_t *base, size_t capacity,
                            int64_t offset, int64_t length,
                            const uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
