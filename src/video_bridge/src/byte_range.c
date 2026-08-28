#include "advc/byte_range.h"

#include <stdint.h>

int advc_checked_byte_range(const uint8_t *base, size_t capacity,
                            int64_t offset, int64_t length,
                            const uint8_t **data, size_t *size) {
    uint64_t unsigned_offset;
    uint64_t unsigned_length;
    size_t checked_offset;
    size_t checked_length;
    if (data == NULL || size == NULL || offset < 0 || length < 0) return -1;
    unsigned_offset = (uint64_t)offset;
    unsigned_length = (uint64_t)length;
    if (unsigned_offset > SIZE_MAX || unsigned_length > SIZE_MAX) return -1;
    checked_offset = (size_t)unsigned_offset;
    checked_length = (size_t)unsigned_length;
    if (checked_offset > capacity || checked_length > capacity - checked_offset)
        return -1;
    if (base == NULL) {
        if (checked_offset != 0 || checked_length != 0) return -1;
        *data = NULL;
    } else {
        *data = base + checked_offset;
    }
    *size = checked_length;
    return 0;
}
