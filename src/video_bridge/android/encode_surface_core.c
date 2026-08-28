#include "encode_surface.h"

#include <stdlib.h>

struct advc_encode_surface {
    void *codec;
    void *window;
    enum advc_encode_surface_route route;
    struct advc_encode_surface_ops ops;
    void *userdata;
    int started;
    int eos;
};

static int ops_are_valid(const struct advc_encode_surface_ops *ops) {
    return ops != NULL && ops->get_api_level != NULL &&
           ops->create_input_surface != NULL && ops->acquire_window != NULL &&
           ops->release_window != NULL &&
           ops->signal_end_of_input_stream != NULL;
}

int advc_encode_surface_route_supported(
    const struct advc_encode_surface_ops *ops,
    enum advc_encode_surface_route route, void *userdata) {
    uint32_t required = ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW |
                        ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY;
    if (!ops_are_valid(ops) || ops->get_api_level(userdata) <
                                   ADVC_ENCODE_SURFACE_MIN_API)
        return 0;
    if (route == ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER) {
        required |= ADVC_ENCODE_SURFACE_FEATURE_AHB_IMPORT |
                    ADVC_ENCODE_SURFACE_FEATURE_EXPLICIT_FENCE;
        if (ops->submit_ahb == NULL || ops->close_fence == NULL) return 0;
    } else if (route != ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW) {
        return 0;
    }
    return (ops->features & required) == required;
}

int advc_encode_surface_create(
    void *codec, enum advc_encode_surface_codec_state codec_state,
    enum advc_encode_surface_route route,
    const struct advc_encode_surface_ops *ops, void *userdata,
    struct advc_encode_surface **surface) {
    struct advc_encode_surface *created;
    void *window = NULL;
    if (surface == NULL || codec == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    *surface = NULL;
    if (codec_state != ADVC_ENCODE_SURFACE_CODEC_CONFIGURED)
        return ADVC_ENCODE_SURFACE_BAD_STATE;
    if (!advc_encode_surface_route_supported(ops, route, userdata))
        return ADVC_ENCODE_SURFACE_UNSUPPORTED;
    created = (struct advc_encode_surface *)calloc(1, sizeof(*created));
    if (created == NULL) return ADVC_ENCODE_SURFACE_PLATFORM_ERROR;
    if (ops->create_input_surface(codec, &window, userdata) != 0 ||
        window == NULL) {
        free(created);
        return ADVC_ENCODE_SURFACE_PLATFORM_ERROR;
    }
    created->codec = codec;
    created->window = window;
    created->route = route;
    created->ops = *ops;
    created->userdata = userdata;
    *surface = created;
    return ADVC_ENCODE_SURFACE_OK;
}

int advc_encode_surface_mark_started(struct advc_encode_surface *surface) {
    if (surface == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    if (surface->started || surface->eos) return ADVC_ENCODE_SURFACE_BAD_STATE;
    surface->started = 1;
    return ADVC_ENCODE_SURFACE_OK;
}

int advc_encode_surface_acquire_window(struct advc_encode_surface *surface,
                                       void **window) {
    if (surface == NULL || window == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    *window = NULL;
    if (surface->eos) return ADVC_ENCODE_SURFACE_BAD_STATE;
    surface->ops.acquire_window(surface->window, surface->userdata);
    *window = surface->window;
    return ADVC_ENCODE_SURFACE_OK;
}

void advc_encode_surface_release_window(struct advc_encode_surface *surface,
                                        void *window) {
    if (surface == NULL || window == NULL || window != surface->window) return;
    surface->ops.release_window(window, surface->userdata);
}

int advc_encode_surface_submit_ahb(struct advc_encode_surface *surface,
                                   const struct advc_encode_ahb_frame *frame,
                                   int *release_fence_fd) {
    int result;
    if (release_fence_fd == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    *release_fence_fd = -1;
    if (surface == NULL || frame == NULL || frame->hardware_buffer == NULL ||
        frame->width == 0 || frame->height == 0 ||
        frame->presentation_time_ns < 0 || frame->acquire_fence_fd < -1)
        return ADVC_ENCODE_SURFACE_INVALID;
    if (surface->route != ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER ||
        surface->ops.submit_ahb == NULL)
        return ADVC_ENCODE_SURFACE_UNSUPPORTED;
    if (!surface->started || surface->eos)
        return ADVC_ENCODE_SURFACE_BAD_STATE;
    result = surface->ops.submit_ahb(surface->window, frame,
                                     release_fence_fd, surface->userdata);
    if (result != 0) {
        if (*release_fence_fd >= 0)
            surface->ops.close_fence(*release_fence_fd, surface->userdata);
        *release_fence_fd = -1;
        return ADVC_ENCODE_SURFACE_PLATFORM_ERROR;
    }
    if (*release_fence_fd < -1) {
        *release_fence_fd = -1;
        return ADVC_ENCODE_SURFACE_PLATFORM_ERROR;
    }
    return ADVC_ENCODE_SURFACE_OK;
}

int advc_encode_surface_signal_eos(struct advc_encode_surface *surface) {
    if (surface == NULL) return ADVC_ENCODE_SURFACE_INVALID;
    if (!surface->started || surface->eos)
        return ADVC_ENCODE_SURFACE_BAD_STATE;
    if (surface->ops.signal_end_of_input_stream(surface->codec,
                                                 surface->userdata) != 0)
        return ADVC_ENCODE_SURFACE_PLATFORM_ERROR;
    surface->eos = 1;
    return ADVC_ENCODE_SURFACE_OK;
}

void advc_encode_surface_destroy(struct advc_encode_surface *surface) {
    if (surface == NULL) return;
    if (surface->window != NULL)
        surface->ops.release_window(surface->window, surface->userdata);
    free(surface);
}
