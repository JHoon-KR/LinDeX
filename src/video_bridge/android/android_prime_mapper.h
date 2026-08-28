#ifndef ADVC_ANDROID_PRIME_MAPPER_H
#define ADVC_ANDROID_PRIME_MAPPER_H

#include "advc/ahb_prime_mapper.h"

/*
 * Loads the Android 15+ stable-C Gralloc5 mapper through the declared
 * passthrough-HAL loader. Export remains fail-closed unless standard metadata
 * provides a complete single-object DRM PRIME description.
 */
struct advc_ahb_prime_mapper *advc_android_prime_mapper_create(void);

struct advc_android_prime_diagnostics {
    uint32_t transport_fds;
    uint32_t transport_ints;
    uint64_t transport_fd_sizes[ADVC_MAX_DMABUF_OBJECTS];
    uint64_t mapper_width;
    uint64_t mapper_height;
    uint64_t mapper_layers;
    uint32_t mapper_stride;
    uint32_t fourcc;
    uint64_t modifier;
    uint64_t allocation_size;
    uint32_t plane_count;
    uint64_t plane_offsets[ADVC_MAX_DMABUF_PLANES];
    uint32_t plane_strides[ADVC_MAX_DMABUF_PLANES];
    uint32_t crop_count;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    int32_t qti_data_fd_value;
    int32_t qti_data_fd_query_errno;
    int32_t qti_data_fd_transport_index;
    uint32_t qti_data_fd_valid;
    uint64_t qti_data_fd_size;
};

const char *advc_android_prime_mapper_last_status(void);
uint32_t advc_android_prime_mapper_last_transport_fds(void);
uint32_t advc_android_prime_mapper_last_transport_ints(void);
void advc_android_prime_mapper_last_diagnostics(
    struct advc_android_prime_diagnostics *diagnostics);

#endif
