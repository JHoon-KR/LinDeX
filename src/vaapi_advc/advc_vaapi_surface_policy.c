#include "advc_vaapi_surface_policy.h"

#include <limits.h>

static int exact_nv12_planes(
    const struct advc_dmabuf_descriptor *descriptor) {
    return descriptor->plane_count == 2 && descriptor->object_count <= 2 &&
           (descriptor->width & 1u) == 0 &&
           (descriptor->height & 1u) == 0 &&
           descriptor->planes[0].pitch >= descriptor->width &&
           descriptor->planes[1].pitch >= descriptor->width;
}

static int linear_nv12_compatible(
    const struct advc_dmabuf_descriptor *descriptor) {
    uint64_t luma_end;
    uint64_t chroma_end;
    const struct advc_dmabuf_plane *luma = &descriptor->planes[0];
    const struct advc_dmabuf_plane *chroma = &descriptor->planes[1];
    if (!exact_nv12_planes(descriptor) ||
        luma->pitch > UINT64_MAX / descriptor->height ||
        chroma->pitch > UINT64_MAX / (descriptor->height / 2u))
        return 0;
    luma_end = luma->offset + (uint64_t)luma->pitch * descriptor->height;
    chroma_end =
        chroma->offset + (uint64_t)chroma->pitch * (descriptor->height / 2u);
    if (luma_end < luma->offset || chroma_end < chroma->offset ||
        luma_end > descriptor->objects[luma->object_index].size ||
        chroma_end > descriptor->objects[chroma->object_index].size)
        return 0;
    if (luma->object_index == chroma->object_index &&
        chroma->offset < luma_end)
        return 0;
    return 1;
}

static int qcom_nv12_compatible(
    const struct advc_dmabuf_descriptor *descriptor) {
    return exact_nv12_planes(descriptor) && descriptor->object_count == 1 &&
           descriptor->planes[0].object_index == 0 &&
           descriptor->planes[1].object_index == 0;
}

enum advc_vaapi_surface_route advc_vaapi_select_surface_route(
    const struct advc_dmabuf_descriptor *descriptor,
    const struct advc_vaapi_consumer_policy *consumer) {
    if (descriptor == NULL || consumer == NULL ||
        advc_dmabuf_descriptor_validate(descriptor) < 0 ||
        descriptor->drm_fourcc != ADVC_DRM_FORMAT_NV12)
        return ADVC_VAAPI_SURFACE_UNSUPPORTED;
    if (descriptor->drm_modifier == 0 &&
        linear_nv12_compatible(descriptor) &&
        consumer->accepts_linear_nv12)
        return ADVC_VAAPI_SURFACE_DIRECT_LINEAR;
    if (descriptor->drm_modifier == ADVC_QCOM_COMPRESSED &&
        qcom_nv12_compatible(descriptor)) {
        if (consumer->accepts_qcom_compressed_nv12)
            return ADVC_VAAPI_SURFACE_DIRECT_QCOM;
        if (consumer->accepts_linear_nv12 && consumer->gpu_repack_available)
            return ADVC_VAAPI_SURFACE_GPU_REPACK_LINEAR;
        if (consumer->accepts_linear_nv12 && consumer->cpu_copy_allowed)
            return ADVC_VAAPI_SURFACE_CPU_COPY_LINEAR;
    }
    return ADVC_VAAPI_SURFACE_UNSUPPORTED;
}
