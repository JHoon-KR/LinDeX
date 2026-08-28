#include "advc_vaapi_decode_repack_vulkan.h"
#include "turnip_prime_import.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)

static unsigned int repack_calls;
static unsigned int close_calls;

static void init_descriptor(struct advc_dmabuf_descriptor *descriptor,
                            uint64_t modifier) {
    memset(descriptor, 0, sizeof(*descriptor));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    descriptor->buffer_id = 77;
    descriptor->width = 16;
    descriptor->height = 16;
    descriptor->drm_fourcc = ADVC_DRM_FORMAT_NV12;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = modifier;
    descriptor->crop_width = 16;
    descriptor->crop_height = 16;
    descriptor->object_count = 1;
    descriptor->plane_count = 2;
    descriptor->objects[0].fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(descriptor->objects[0].fd >= 0);
    descriptor->objects[0].size = 384;
    descriptor->planes[0].pitch = 16;
    descriptor->planes[1].offset = 256;
    descriptor->planes[1].pitch = 16;
    assert(advc_dmabuf_descriptor_validate(descriptor) == 0);
}

int advc_turnip_prime_repack_linear(
    const struct advc_dmabuf_descriptor *source,
    int source_acquire_fence_fd, uint64_t output_buffer_id,
    struct advc_turnip_linear_repack_result *result) {
    (void)source_acquire_fence_fd;
    ++repack_calls;
    assert(source != NULL && output_buffer_id == source->buffer_id);
    init_descriptor(&result->descriptor, 0);
    result->descriptor.buffer_id = output_buffer_id;
    result->acquire_fence_fd = -1;
    result->source_release_fence_fd = -1;
    return 0;
}

void advc_turnip_linear_repack_close(
    struct advc_turnip_linear_repack_result *result) {
    ++close_calls;
    advc_dmabuf_descriptor_close(&result->descriptor);
    if (result->acquire_fence_fd >= 0) close(result->acquire_fence_fd);
    if (result->source_release_fence_fd >= 0)
        close(result->source_release_fence_fd);
}

static void reset_outputs(struct advc_dmabuf_descriptor *linear,
                          int *acquire, int *release) {
    memset(linear, 0, sizeof(*linear));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        linear->objects[i].fd = -1;
    *acquire = -1;
    *release = -1;
}

int main(void) {
    struct advc_dmabuf_descriptor source;
    struct advc_dmabuf_descriptor linear;
    int acquire = -1;
    int release = -1;

    init_descriptor(&source, ADVC_QCOM_COMPRESSED);
    unsetenv("ADVC_VAAPI_GPU_LINEAR_REPACK");
    reset_outputs(&linear, &acquire, &release);
    errno = 0;
    assert(advc_vaapi_gpu_repack_linear(&source, -1, &linear, &acquire,
                                        &release) < 0);
    assert(errno == ENOTSUP && repack_calls == 0);

    setenv("ADVC_VAAPI_GPU_LINEAR_REPACK", "1", 1);
    errno = 0;
    assert(advc_vaapi_gpu_repack_linear(&source, -1, &linear, &acquire,
                                        &release) < 0);
    assert(errno == ENOTSUP && repack_calls == 0);

    setenv("ADVC_VAAPI_GPU_LINEAR_REPACK", "validated-qcom-nv12-v1", 1);
    source.drm_modifier = 0;
    errno = 0;
    assert(advc_vaapi_gpu_repack_linear(&source, -1, &linear, &acquire,
                                        &release) < 0);
    assert(errno == EINVAL && repack_calls == 0);

    source.drm_modifier = ADVC_QCOM_COMPRESSED;
    assert(advc_vaapi_gpu_repack_linear(&source, -1, &linear, &acquire,
                                        &release) == 0);
    assert(repack_calls == 1 && close_calls == 1);
    assert(linear.buffer_id == source.buffer_id);
    assert(linear.drm_fourcc == ADVC_DRM_FORMAT_NV12);
    assert(linear.drm_modifier == 0);
    assert(advc_dmabuf_descriptor_validate(&linear) == 0);
    assert(acquire == -1 && release == -1);
    assert(fcntl(source.objects[0].fd, F_GETFD) >= 0);

    advc_dmabuf_descriptor_close(&linear);
    advc_dmabuf_descriptor_close(&source);
    puts("advc VA-API Vulkan decode repack gate: PASS");
    return 0;
}
