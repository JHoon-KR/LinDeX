#include "encode_surface.h"
#include "encode_surface_egl.h"
#include "encode_surface_vulkan.h"
#include "surface_encode_probe.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct advc_encode_surface { int started; int eos; };
struct advc_egl_surface_producer { int unused; };
struct advc_vk_surface_producer { int unused; };

static int render_calls;
static uint64_t last_frame;
static int64_t last_pts;
static int eos_calls;

void advc_test_surface_reset(void) {
    render_calls = 0;
    last_frame = 0;
    last_pts = 0;
    eos_calls = 0;
}
int advc_test_surface_render_calls(void) { return render_calls; }
uint64_t advc_test_surface_last_frame(void) { return last_frame; }
int64_t advc_test_surface_last_pts(void) { return last_pts; }
int advc_test_surface_eos_calls(void) { return eos_calls; }

const struct advc_encode_surface_ops *advc_encode_surface_ndk_ops(void) {
    static const struct advc_encode_surface_ops ops = {0};
    return &ops;
}

int advc_encode_surface_create(
    void *codec, enum advc_encode_surface_codec_state codec_state,
    enum advc_encode_surface_route route,
    const struct advc_encode_surface_ops *ops, void *userdata,
    struct advc_encode_surface **surface) {
    (void)ops;
    (void)userdata;
    if (codec == NULL || codec_state != ADVC_ENCODE_SURFACE_CODEC_CONFIGURED ||
        route != ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW || surface == NULL)
        return ADVC_ENCODE_SURFACE_UNSUPPORTED;
    *surface = calloc(1, sizeof(**surface));
    return *surface == NULL ? ADVC_ENCODE_SURFACE_PLATFORM_ERROR :
                              ADVC_ENCODE_SURFACE_OK;
}

int advc_encode_surface_acquire_window(struct advc_encode_surface *surface,
                                       void **window) {
    if (surface == NULL || window == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    *window = (void *)(uintptr_t)0x3000;
    return ADVC_ENCODE_SURFACE_OK;
}

void advc_encode_surface_release_window(struct advc_encode_surface *surface,
                                        void *window) {
    (void)surface;
    (void)window;
}

int advc_encode_surface_mark_started(struct advc_encode_surface *surface) {
    if (surface == NULL || surface->started) return ADVC_ENCODE_SURFACE_BAD_STATE;
    surface->started = 1;
    return ADVC_ENCODE_SURFACE_OK;
}

int advc_encode_surface_signal_eos(struct advc_encode_surface *surface) {
    if (surface == NULL || !surface->started || surface->eos)
        return ADVC_ENCODE_SURFACE_BAD_STATE;
    surface->eos = 1;
    ++eos_calls;
    return ADVC_ENCODE_SURFACE_OK;
}

void advc_encode_surface_destroy(struct advc_encode_surface *surface) {
    free(surface);
}

int advc_egl_surface_producer_create(void *native_window, uint32_t width,
                                     uint32_t height,
                                     struct advc_egl_surface_producer **producer) {
    if (native_window == NULL || width == 0 || height == 0 || producer == NULL)
        return -1;
    *producer = calloc(1, sizeof(**producer));
    return *producer == NULL ? -1 : 0;
}

int advc_egl_surface_producer_render(struct advc_egl_surface_producer *producer,
                                     uint64_t frame_sequence,
                                     int64_t presentation_time_ns) {
    if (producer == NULL || presentation_time_ns < 0)
        return -1;
    ++render_calls;
    last_frame = frame_sequence;
    last_pts = presentation_time_ns;
    return 0;
}

int advc_egl_surface_producer_discard_import_caches(
    struct advc_egl_surface_producer *producer) {
    return producer == NULL ? -1 : 0;
}

void advc_egl_surface_producer_destroy(struct advc_egl_surface_producer *producer) {
    free(producer);
}

int advc_android_dmabuf_surface_route(void) {
    return ADVC_DMABUF_SURFACE_NONE;
}

int advc_probe_android_dmabuf_surface_backend(int route) {
    (void)route;
    return 0;
}

const char *advc_android_dmabuf_surface_status(void) {
    return "selected=none;reason=test-stub";
}

const char *advc_android_dmabuf_surface_backend_status(int route) {
    (void)route;
    return "test-stub";
}

int advc_vk_surface_producer_create(
    void *native_window, uint32_t width, uint32_t height,
    struct advc_vk_surface_producer **producer) {
    (void)native_window;
    (void)width;
    (void)height;
    (void)producer;
    return -1;
}

int advc_vk_surface_producer_validate_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor) {
    (void)producer;
    (void)descriptor;
    return -1;
}

int advc_vk_surface_producer_render_dmabuf(
    struct advc_vk_surface_producer *producer,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t frame_sequence,
    int64_t presentation_time_ns, int acquire_fence_fd,
    int *release_fence_fd) {
    (void)producer;
    (void)descriptor;
    (void)frame_sequence;
    (void)presentation_time_ns;
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    if (release_fence_fd != NULL) *release_fence_fd = -1;
    return -1;
}

void advc_vk_surface_producer_destroy(struct advc_vk_surface_producer *producer) {
    free(producer);
}
