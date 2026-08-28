#ifndef ADVC_ENCODE_SURFACE_EGL_H
#define ADVC_ENCODE_SURFACE_EGL_H

#include "advc/dmabuf_ingress.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct advc_egl_surface_producer;

int advc_egl_surface_producer_create(void *native_window, uint32_t width,
                                     uint32_t height,
                                     struct advc_egl_surface_producer **producer);
int advc_egl_surface_producer_render(struct advc_egl_surface_producer *producer,
                                     uint64_t frame_sequence,
                                     int64_t presentation_time_ns);
/* Consumes acquire_fence_fd on every return; caller owns release fence on success. */
int advc_egl_surface_producer_render_ahb(
    struct advc_egl_surface_producer *producer, void *hardware_buffer,
    uint64_t frame_sequence, int64_t presentation_time_ns,
    int acquire_fence_fd, int *release_fence_fd);
/*
 * The exact descriptor is imported before registration is accepted. Runtime
 * submission performs zero CPU pixel copies and exactly one GPU draw into the
 * MediaCodec Surface.
 */
int advc_egl_surface_producer_validate_dmabuf(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor);
int advc_egl_surface_producer_render_dmabuf(
    struct advc_egl_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t frame_sequence,
    int64_t presentation_time_ns, int acquire_fence_fd,
    int *release_fence_fd);
/* Bounded fence wait with synchronous fail-safe; used by flush/EOS teardown. */
int advc_egl_surface_producer_discard_import_caches(
    struct advc_egl_surface_producer *producer);
void advc_egl_surface_producer_destroy(struct advc_egl_surface_producer *producer);

#ifdef __cplusplus
}
#endif

#endif
