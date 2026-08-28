#ifndef ADVC_ANDROID_AHB_DECODE_H
#define ADVC_ANDROID_AHB_DECODE_H

#include <stdint.h>

struct advc_ahb_surface;

int advc_ahb_surface_create(uint32_t width, uint32_t height, uint32_t max_images,
                            struct advc_ahb_surface **surface,
                            void **native_window);
void advc_ahb_surface_destroy(struct advc_ahb_surface *surface);
/* Returns 0, EAGAIN-like 1 when no image is ready, or -1. */
int advc_ahb_surface_acquire(struct advc_ahb_surface *surface, void **image,
                             void **hardware_buffer, int *acquire_fence_fd,
                             int64_t *timestamp_ns, uint32_t *width,
                             uint32_t *height, uint32_t *layers,
                             uint32_t *format, uint32_t *stride,
                             uint64_t *usage);
void advc_ahb_surface_discard_available(struct advc_ahb_surface *surface);
/* Consumes release_fence_fd when nonnegative. */
void advc_ahb_surface_release(void *image, int release_fence_fd);
int advc_ahb_send(int socket_fd, void *hardware_buffer);

#endif
