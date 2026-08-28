#include "advc_vaapi_decode_repack_vulkan.h"

#include "turnip_prime_import.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)

static int exact_env(const char *name, const char *expected) {
    const char *actual = getenv(name);
    return actual != NULL && strcmp(actual, expected) == 0;
}

int advc_vaapi_gpu_repack_linear(
    const struct advc_dmabuf_descriptor *source, int source_acquire_fence_fd,
    struct advc_dmabuf_descriptor *linear, int *linear_acquire_fence_fd,
    int *source_release_fence_fd) {
    struct advc_turnip_linear_repack_result result;

    if (linear == NULL || linear_acquire_fence_fd == NULL ||
        source_release_fence_fd == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(linear, 0, sizeof(*linear));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        linear->objects[i].fd = -1;
    *linear_acquire_fence_fd = -1;
    *source_release_fence_fd = -1;

    if (!exact_env("ADVC_VAAPI_GPU_LINEAR_REPACK",
                   "validated-qcom-nv12-v1")) {
        errno = ENOTSUP;
        return -1;
    }
    if (source == NULL || source_acquire_fence_fd < -1 ||
        source->buffer_id == 0 ||
        advc_dmabuf_descriptor_validate(source) < 0 ||
        source->drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        source->drm_modifier != ADVC_QCOM_COMPRESSED) {
        errno = EINVAL;
        return -1;
    }

    memset(&result, 0, sizeof(result));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        result.descriptor.objects[i].fd = -1;
    result.acquire_fence_fd = -1;
    result.source_release_fence_fd = -1;
    if (advc_turnip_prime_repack_linear(
            source, source_acquire_fence_fd, source->buffer_id, &result) < 0)
        return -1;

    if (result.descriptor.drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        result.descriptor.drm_modifier != 0 ||
        advc_dmabuf_descriptor_validate(&result.descriptor) < 0) {
        advc_turnip_linear_repack_close(&result);
        errno = EPROTO;
        return -1;
    }

    *linear = result.descriptor;
    memset(&result.descriptor, 0, sizeof(result.descriptor));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        result.descriptor.objects[i].fd = -1;
    *linear_acquire_fence_fd = result.acquire_fence_fd;
    result.acquire_fence_fd = -1;
    *source_release_fence_fd = result.source_release_fence_fd;
    result.source_release_fence_fd = -1;
    advc_turnip_linear_repack_close(&result);
    return 0;
}
