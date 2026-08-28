#include "android_ahb_decode.h"

#include <stddef.h>
#include <stdint.h>

static int acquire_calls;
static int release_calls;
static int discard_calls;

void advc_test_ahb_reset(void) {
    acquire_calls = 0;
    release_calls = 0;
    discard_calls = 0;
}

int advc_test_ahb_acquire_calls(void) { return acquire_calls; }
int advc_test_ahb_release_calls(void) { return release_calls; }
int advc_test_ahb_discard_calls(void) { return discard_calls; }

int advc_ahb_surface_create(uint32_t width, uint32_t height, uint32_t max_images,
                            struct advc_ahb_surface **surface, void **native_window) {
    (void)width; (void)height; (void)max_images;
    if (surface == NULL || native_window == NULL) return -1;
    *surface = (struct advc_ahb_surface *)(uintptr_t)0x1000;
    *native_window = (void *)(uintptr_t)0x2000;
    return 0;
}
void advc_ahb_surface_destroy(struct advc_ahb_surface *surface) { (void)surface; }
int advc_ahb_surface_acquire(struct advc_ahb_surface *surface, void **image,
                             void **hardware_buffer, int *acquire_fence_fd,
                             int64_t *timestamp_ns, uint32_t *width,
                             uint32_t *height, uint32_t *layers,
                             uint32_t *format, uint32_t *stride,
                             uint64_t *usage) {
    (void)surface; (void)image; (void)hardware_buffer;
    (void)acquire_fence_fd; (void)timestamp_ns; (void)width; (void)height;
    (void)layers; (void)format; (void)stride; (void)usage;
    ++acquire_calls;
    return 1;
}
void advc_ahb_surface_discard_available(struct advc_ahb_surface *surface) {
    (void)surface;
    ++discard_calls;
}
void advc_ahb_surface_release(void *image, int release_fence_fd) {
    (void)image; (void)release_fence_fd;
    ++release_calls;
}
int advc_ahb_send(int socket_fd, void *hardware_buffer) {
    (void)socket_fd; (void)hardware_buffer; return -1;
}
