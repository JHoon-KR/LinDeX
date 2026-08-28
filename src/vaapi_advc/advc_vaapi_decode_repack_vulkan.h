#ifndef ADVC_VAAPI_DECODE_REPACK_VULKAN_H
#define ADVC_VAAPI_DECODE_REPACK_VULKAN_H

#include "advc/dmabuf_ingress.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime hook consumed by advc_vaapi_decode_runtime.c. The source descriptor
 * and source acquire fence are borrowed. On success the caller owns the
 * returned LINEAR descriptor and both returned sync_file FDs.
 *
 * This path is deliberately unavailable unless
 * ADVC_VAAPI_GPU_LINEAR_REPACK=validated-qcom-nv12-v1 is an exact match.
 */
int advc_vaapi_gpu_repack_linear(
    const struct advc_dmabuf_descriptor *source, int source_acquire_fence_fd,
    struct advc_dmabuf_descriptor *linear, int *linear_acquire_fence_fd,
    int *source_release_fence_fd);

#ifdef __cplusplus
}
#endif

#endif
