#include "advc_vaapi_slice_layout.h"

#include <errno.h>
#include <limits.h>

int advc_vaapi_rebase_slice_offsets(uint32_t *offsets,
                                    const uint32_t *sizes,
                                    size_t span_count,
                                    size_t aggregate_size,
                                    size_t local_data_size,
                                    size_t maximum_size) {
    size_t i;

    if (offsets == NULL || sizes == NULL || span_count == 0 ||
        local_data_size == 0 || aggregate_size > maximum_size ||
        local_data_size > maximum_size - aggregate_size ||
        aggregate_size + local_data_size > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < span_count; ++i) {
        size_t offset = offsets[i];
        size_t size = sizes[i];
        if (offset > local_data_size || size == 0 ||
            size > local_data_size - offset ||
            aggregate_size + offset > UINT32_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    for (i = 0; i < span_count; ++i)
        offsets[i] = (uint32_t)(aggregate_size + offsets[i]);
    return 0;
}
