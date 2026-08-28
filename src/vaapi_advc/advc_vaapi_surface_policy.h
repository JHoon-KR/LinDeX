#ifndef ADVC_VAAPI_SURFACE_POLICY_H
#define ADVC_VAAPI_SURFACE_POLICY_H

#include "advc/dmabuf_ingress.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)

enum advc_vaapi_surface_route {
    ADVC_VAAPI_SURFACE_UNSUPPORTED = 0,
    ADVC_VAAPI_SURFACE_DIRECT_LINEAR = 1,
    ADVC_VAAPI_SURFACE_DIRECT_QCOM = 2,
    ADVC_VAAPI_SURFACE_GPU_REPACK_LINEAR = 3,
    ADVC_VAAPI_SURFACE_CPU_COPY_LINEAR = 4,
};

struct advc_vaapi_consumer_policy {
    int accepts_linear_nv12;
    int accepts_qcom_compressed_nv12;
    int gpu_repack_available;
    int cpu_copy_allowed;
};

/*
 * Route order is strict: compatible original LINEAR, compatible original
 * QCOM compressed, validated GPU repack, explicit CPU fallback, unsupported.
 */
enum advc_vaapi_surface_route advc_vaapi_select_surface_route(
    const struct advc_dmabuf_descriptor *descriptor,
    const struct advc_vaapi_consumer_policy *consumer);

#ifdef __cplusplus
}
#endif

#endif
