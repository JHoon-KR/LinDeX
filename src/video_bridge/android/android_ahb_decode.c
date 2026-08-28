#include "android_ahb_decode.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <stdlib.h>
#include <string.h>

struct advc_ahb_surface {
    AImageReader *reader;
    uint32_t max_images;
};

int advc_ahb_surface_create(uint32_t width, uint32_t height, uint32_t max_images,
                            struct advc_ahb_surface **surface,
                            void **native_window) {
    struct advc_ahb_surface *created;
    ANativeWindow *window = NULL;
    int32_t image_format = AIMAGE_FORMAT_PRIVATE;
    uint64_t image_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                           AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    media_status_t status;
    if (surface == NULL || native_window == NULL || width == 0 || height == 0 ||
        max_images == 0) return -1;
    *surface = NULL;
    *native_window = NULL;
    created = (struct advc_ahb_surface *)calloc(1, sizeof(*created));
    if (created == NULL) return -1;
    {
        const char *linear_validation =
            getenv("ADVC_DECODE_LINEAR_VALIDATION");
        if (linear_validation != NULL &&
            strcmp(linear_validation, "turnip-offscreen-only") == 0) {
            /*
             * Validation-only request for a real, mapper-described LINEAR
             * decoder allocation. Never relabel a PRIVATE/UBWC allocation as
             * LINEAR. CPU usage is an allocation constraint only; LinDeX does
             * not map or copy the returned pixels on this path.
             */
            image_format = AIMAGE_FORMAT_YUV_420_888;
            image_usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        }
    }
    status = AImageReader_newWithUsage(
        (int32_t)width, (int32_t)height, image_format, image_usage,
        (int32_t)max_images, &created->reader);
    if (status != AMEDIA_OK || created->reader == NULL ||
        AImageReader_getWindow(created->reader, &window) != AMEDIA_OK ||
        window == NULL) {
        if (created->reader != NULL) AImageReader_delete(created->reader);
        free(created);
        return -1;
    }
    *surface = created;
    *native_window = window;
    created->max_images = max_images;
    return 0;
}

void advc_ahb_surface_destroy(struct advc_ahb_surface *surface) {
    if (surface == NULL) return;
    if (surface->reader != NULL) AImageReader_delete(surface->reader);
    free(surface);
}

int advc_ahb_surface_acquire(struct advc_ahb_surface *surface, void **image,
                             void **hardware_buffer, int *acquire_fence_fd,
                             int64_t *timestamp_ns, uint32_t *width,
                             uint32_t *height, uint32_t *layers,
                             uint32_t *format, uint32_t *stride,
                             uint64_t *usage) {
    AImage *acquired = NULL;
    AHardwareBuffer *buffer = NULL;
    int fence = -1;
    media_status_t status;
    if (surface == NULL || image == NULL || hardware_buffer == NULL ||
        acquire_fence_fd == NULL || timestamp_ns == NULL || width == NULL ||
        height == NULL || layers == NULL || format == NULL || stride == NULL ||
        usage == NULL) return -1;
    *image = NULL;
    *hardware_buffer = NULL;
    *acquire_fence_fd = -1;
    *timestamp_ns = 0;
    status = AImageReader_acquireNextImageAsync(surface->reader, &acquired, &fence);
    if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) return 1;
    if (status != AMEDIA_OK || acquired == NULL) return -1;
    if (AImage_getHardwareBuffer(acquired, &buffer) != AMEDIA_OK || buffer == NULL ||
        AImage_getTimestamp(acquired, timestamp_ns) != AMEDIA_OK) {
        AImage_deleteAsync(acquired, fence);
        return -1;
    }
    {
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(buffer, &desc);
        *width = desc.width;
        *height = desc.height;
        *layers = desc.layers;
        *format = desc.format;
        *stride = desc.stride;
        *usage = desc.usage;
    }
    *image = acquired;
    *hardware_buffer = buffer;
    *acquire_fence_fd = fence;
    return 0;
}

void advc_ahb_surface_discard_available(struct advc_ahb_surface *surface) {
    uint32_t discarded;
    if (surface == NULL) return;
    for (discarded = 0; discarded < surface->max_images; ++discarded) {
        void *image = NULL;
        void *buffer = NULL;
        int fence = -1;
        int64_t timestamp = 0;
        uint32_t width, height, layers, format, stride;
        uint64_t usage;
        int result = advc_ahb_surface_acquire(surface, &image, &buffer, &fence,
                                              &timestamp, &width, &height, &layers,
                                              &format, &stride, &usage);
        if (result != 0) return;
        AImage_deleteAsync((AImage *)image, fence);
    }
}

void advc_ahb_surface_release(void *image, int release_fence_fd) {
    if (image != NULL) AImage_deleteAsync((AImage *)image, release_fence_fd);
}

int advc_ahb_send(int socket_fd, void *hardware_buffer) {
    if (hardware_buffer == NULL) return -1;
    return AHardwareBuffer_sendHandleToUnixSocket(
        (const AHardwareBuffer *)hardware_buffer, socket_fd);
}
