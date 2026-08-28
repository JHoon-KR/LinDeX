#include "advc_vaapi_surface_policy.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct advc_dmabuf_descriptor descriptor(uint64_t modifier, int fd) {
    struct advc_dmabuf_descriptor value;
    memset(&value, 0, sizeof(value));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        value.objects[i].fd = -1;
    value.buffer_id = 1;
    value.width = 128;
    value.height = 64;
    value.drm_fourcc = ADVC_DRM_FORMAT_NV12;
    value.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    value.drm_modifier = modifier;
    value.crop_width = 128;
    value.crop_height = 64;
    value.object_count = 1;
    value.plane_count = 2;
    value.objects[0].fd = fd;
    value.objects[0].size = 12288;
    value.planes[0].pitch = 128;
    value.planes[1].object_index = 0;
    value.planes[1].offset = 8192;
    value.planes[1].pitch = 128;
    return value;
}

int main(void) {
    struct advc_vaapi_consumer_policy consumer = {0};
    struct advc_dmabuf_descriptor linear;
    struct advc_dmabuf_descriptor qcom;
    struct advc_dmabuf_descriptor unknown;
    struct advc_dmabuf_descriptor malformed;
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    linear = descriptor(0, fd);
    qcom = descriptor(ADVC_QCOM_COMPRESSED, fd);
    unknown = descriptor(UINT64_C(0x0100000000000001), fd);
    malformed = linear;
    malformed.plane_count = 1;
    memset(&malformed.planes[1], 0, sizeof(malformed.planes[1]));

    consumer.accepts_linear_nv12 = 1;
    consumer.accepts_qcom_compressed_nv12 = 1;
    consumer.gpu_repack_available = 1;
    consumer.cpu_copy_allowed = 1;
    assert(advc_vaapi_select_surface_route(&linear, &consumer) ==
           ADVC_VAAPI_SURFACE_DIRECT_LINEAR);
    assert(advc_vaapi_select_surface_route(&qcom, &consumer) ==
           ADVC_VAAPI_SURFACE_DIRECT_QCOM);
    consumer.accepts_qcom_compressed_nv12 = 0;
    assert(advc_vaapi_select_surface_route(&qcom, &consumer) ==
           ADVC_VAAPI_SURFACE_GPU_REPACK_LINEAR);
    consumer.gpu_repack_available = 0;
    assert(advc_vaapi_select_surface_route(&qcom, &consumer) ==
           ADVC_VAAPI_SURFACE_CPU_COPY_LINEAR);
    consumer.cpu_copy_allowed = 0;
    assert(advc_vaapi_select_surface_route(&qcom, &consumer) ==
           ADVC_VAAPI_SURFACE_UNSUPPORTED);
    consumer.gpu_repack_available = 1;
    consumer.cpu_copy_allowed = 1;
    assert(advc_vaapi_select_surface_route(&unknown, &consumer) ==
           ADVC_VAAPI_SURFACE_UNSUPPORTED);
    assert(advc_vaapi_select_surface_route(&malformed, &consumer) ==
           ADVC_VAAPI_SURFACE_UNSUPPORTED);
    close(fd);
    puts("advc VA-API surface policy: PASS");
    return 0;
}
