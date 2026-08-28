#ifndef ADVC_ENCODE_SURFACE_H
#define ADVC_ENCODE_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_ENCODE_SURFACE_MIN_API 26

enum advc_encode_surface_route {
    ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW = 1,
    ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER = 2,
};

enum advc_encode_surface_feature {
    ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW = 1u << 0,
    ADVC_ENCODE_SURFACE_FEATURE_AHB_IMPORT = 1u << 1,
    ADVC_ENCODE_SURFACE_FEATURE_EXPLICIT_FENCE = 1u << 2,
    ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY = 1u << 3,
};

enum advc_encode_surface_codec_state {
    ADVC_ENCODE_SURFACE_CODEC_UNCONFIGURED = 0,
    ADVC_ENCODE_SURFACE_CODEC_CONFIGURED = 1,
    ADVC_ENCODE_SURFACE_CODEC_STARTED = 2,
};

enum advc_encode_surface_result {
    ADVC_ENCODE_SURFACE_OK = 0,
    ADVC_ENCODE_SURFACE_INVALID = -1,
    ADVC_ENCODE_SURFACE_UNSUPPORTED = -2,
    ADVC_ENCODE_SURFACE_PLATFORM_ERROR = -3,
    ADVC_ENCODE_SURFACE_BAD_STATE = -4,
};

struct advc_encode_surface;

struct advc_encode_ahb_frame {
    void *hardware_buffer;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t usage;
    int64_t presentation_time_ns;
    int acquire_fence_fd;
};

/*
 * submit_ahb is deliberately optional. The public NDK cannot attach an
 * arbitrary AHardwareBuffer to a MediaCodec input ANativeWindow. A backend may
 * provide this operation only when it has a real EGL/Vulkan import-and-render
 * implementation. Once called, it consumes frame->acquire_fence_fd even when
 * it fails. On success, release_fence_fd is owned by the caller; -1 means the
 * buffer is already safe to reuse. It must not return a fence on failure.
 */
struct advc_encode_surface_ops {
    uint32_t features;
    int (*get_api_level)(void *userdata);
    int (*create_input_surface)(void *codec, void **window, void *userdata);
    void (*acquire_window)(void *window, void *userdata);
    void (*release_window)(void *window, void *userdata);
    int (*signal_end_of_input_stream)(void *codec, void *userdata);
    int (*submit_ahb)(void *window, const struct advc_encode_ahb_frame *frame,
                      int *release_fence_fd, void *userdata);
    void (*close_fence)(int fence_fd, void *userdata);
};

int advc_encode_surface_route_supported(
    const struct advc_encode_surface_ops *ops,
    enum advc_encode_surface_route route, void *userdata);

/* MediaCodec must be configured as an encoder and must not have been started. */
int advc_encode_surface_create(
    void *codec, enum advc_encode_surface_codec_state codec_state,
    enum advc_encode_surface_route route,
    const struct advc_encode_surface_ops *ops, void *userdata,
    struct advc_encode_surface **surface);

int advc_encode_surface_mark_started(struct advc_encode_surface *surface);

/* Returns a separately acquired window reference. */
int advc_encode_surface_acquire_window(struct advc_encode_surface *surface,
                                       void **window);
void advc_encode_surface_release_window(struct advc_encode_surface *surface,
                                        void *window);

int advc_encode_surface_submit_ahb(struct advc_encode_surface *surface,
                                   const struct advc_encode_ahb_frame *frame,
                                   int *release_fence_fd);
int advc_encode_surface_signal_eos(struct advc_encode_surface *surface);
void advc_encode_surface_destroy(struct advc_encode_surface *surface);

/* Stable NDK implementation: direct codec Surface input only. */
const struct advc_encode_surface_ops *advc_encode_surface_ndk_ops(void);

#ifdef __cplusplus
}
#endif

#endif
