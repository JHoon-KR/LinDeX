#ifndef ADVC_ENCODE_SURFACE_VULKAN_H
#define ADVC_ENCODE_SURFACE_VULKAN_H

#include "advc/dmabuf_ingress.h"

#include <stdint.h>

struct advc_vk_surface_producer;

int advc_vk_surface_producer_create(
    void *native_window, uint32_t width, uint32_t height,
    struct advc_vk_surface_producer **producer);
int advc_vk_surface_producer_validate_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor);
/* Consumes acquire_fence_fd on every return; caller owns release fence. */
int advc_vk_surface_producer_render_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t frame_sequence,
    int64_t presentation_time_ns, int acquire_fence_fd,
    int *release_fence_fd);
void advc_vk_surface_producer_destroy(struct advc_vk_surface_producer *producer);

#endif
