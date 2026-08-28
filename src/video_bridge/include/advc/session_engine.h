#ifndef ADVC_SESSION_ENGINE_H
#define ADVC_SESSION_ENGINE_H

#include "advc/ahb_prime_mapper.h"
#include "advc/dmabuf_ingress.h"
#include "advc/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct advc_backend_config {
    uint8_t direction;
    char mime[ADVC_MAX_MIME];
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    uint32_t framerate_milli;
    uint32_t color_format;
    uint32_t transport;
    uint32_t encode_profile;
    uint32_t flags;
};

struct advc_backend_output {
    const uint8_t *data;
    size_t size;
    uint64_t pts_ns;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t android_format;
    uint32_t stride;
    uint32_t layers;
    uint64_t usage;
    uint32_t slice_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_right;
    uint32_t crop_bottom;
    uintptr_t token;
    void *native_buffer;
    int acquire_fence_fd;
    uint32_t transport;
};

struct advc_backend_ahb_input {
    uint64_t pts_ns;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t layers;
    uint64_t usage;
    int acquire_fence_fd; /* Consumed by receive_ahb_input on every return. */
};

/* receive_ahb_input only: the native handle record was absent or malformed. */
#define ADVC_BACKEND_AHB_FATAL_TRANSPORT UINT32_MAX

struct advc_backend_ops {
    uint32_t (*create)(void *userdata, const struct advc_backend_config *config,
                       void **handle);
    uint32_t (*queue_input)(void *userdata, void *handle, const uint8_t *data,
                            size_t size, uint64_t pts_ns, uint32_t flags);
    uint32_t (*dequeue_output)(void *userdata, void *handle,
                              struct advc_backend_output *output);
    void (*release_output)(void *userdata, void *handle, uintptr_t token,
                           int release_fence_fd);
    /*
     * Export authoritative PRIME metadata and owned dma-buf object FDs for a
     * dequeued AHB. The acquire fence remains in DEQUEUE_OUTPUT; this callback
     * must not consume the AHB or retire the codec output. On every return the
     * descriptor must remain close-safe: owned FDs are unique and contiguous
     * in objects[0..object_count), and object_count is published before an
     * operation that can fail. The caller closes those declared FDs even when
     * the callback returns an error.
     */
    uint32_t (*export_decode_prime)(
        void *userdata, void *handle, void *native_buffer,
        const struct advc_ahb_public_metadata *metadata, uint64_t buffer_id,
        struct advc_dmabuf_descriptor *descriptor);
    int (*send_native_buffer)(void *userdata, int socket_fd, void *native_buffer);
    uint32_t (*receive_ahb_input)(void *userdata, void *handle, int socket_fd,
                                 const struct advc_backend_ahb_input *input,
                                 int *release_fence_fd);
    /*
     * A successful format check means that this exact descriptor can be
     * imported by the active Android EGL/Vulkan implementation. It must not
     * infer missing modifier or plane metadata.
     */
    int (*dmabuf_format_allowed)(
        void *userdata, void *handle,
        const struct advc_dmabuf_descriptor *descriptor);
    /*
     * CPU-pixel-copy-free import followed by exactly one bounded GPU draw into
     * the codec Surface. The backend consumes acquire_fence_fd on every return.
     * A release fence is returned only with ADVC_STATUS_OK; -1 means the read
     * was synchronously quiesced before return.
     */
    uint32_t (*submit_dmabuf)(
        void *userdata, void *handle,
        const struct advc_dmabuf_descriptor *descriptor, uint64_t pts_ns,
        int acquire_fence_fd, int *release_fence_fd);
    uint32_t (*flush)(void *userdata, void *handle);
    void (*destroy)(void *userdata, void *handle);
};

struct advc_session_engine;

struct advc_session_engine *advc_session_engine_create(const struct advc_backend_ops *ops,
                                                       void *backend_userdata);
void advc_session_engine_destroy(struct advc_session_engine *engine);

/* Compatible with advc_broker_provider.handle_codec_request. */
uint32_t advc_session_engine_handle(void *userdata, const struct advc_message *request,
                                    struct advc_message *reply);
/* Runs only after the matching protocol reply has been sent. */
int advc_session_engine_after_reply(void *userdata, int client_fd,
                                    const struct advc_message *request,
                                    const struct advc_message *reply);

#ifdef __cplusplus
}
#endif

#endif
