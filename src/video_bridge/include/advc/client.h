#ifndef ADVC_CLIENT_H
#define ADVC_CLIENT_H

#include "advc/capabilities.h"
#include "advc/dmabuf_ingress.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_CLIENT_CONNECT_TIMEOUT_MS 5000u

int advc_client_connect(const char *socket_path);
/* Bounds the AF_UNIX connect phase itself with a monotonic deadline. */
int advc_client_connect_bounded(const char *socket_path, uint32_t timeout_ms);
int advc_client_recycle_broker_socket(int fd);
int advc_client_hello(int fd, uint64_t client_features, uint64_t *negotiated_features,
                      uint32_t *max_payload);
int advc_client_query_capabilities(int fd, struct advc_capability_set *caps);

/*
 * Session calls return an advc_status value, or -1 for a local transport or
 * malformed-response failure (with errno set). The caller retains ownership of
 * every input pointer and input fd passed to these functions.
 */
struct advc_client_session_config {
    const char *mime;
    uint32_t direction;
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    uint32_t framerate_milli;
    uint32_t color_format;
    uint32_t transport; /* One ADVC_TRANSPORT_* value; never an implicit fallback. */
    uint32_t encode_profile; /* One ADVC_ENCODE_PROFILE_* value; zero for decode. */
    uint32_t flags;
};

struct advc_client_input {
    const void *data;
    size_t size;
    int data_fd;             /* -1 for inline data; otherwise a sealed memfd. */
    uint64_t data_offset;    /* Used only when data_fd is nonnegative. */
    uint64_t buffer_id;
    uint64_t pts_ns;
    uint32_t flags;
};

struct advc_client_output {
    uint64_t buffer_id;
    uint64_t pts_ns;
    uint64_t size;
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
    int data_fd; /* Owned by the caller after successful dequeue; close it. */
    uint32_t transport;
    int acquire_fence_fd; /* Owned by caller; -1 when absent. */
};

typedef int (*advc_receive_native_buffer_fn)(int socket_fd, void **native_buffer,
                                             void *userdata);
typedef int (*advc_send_native_buffer_fn)(int socket_fd, void *native_buffer,
                                          void *userdata);

struct advc_client_ahb_input {
    void *native_buffer; /* Android AHardwareBuffer; never interpreted by glibc. */
    uint64_t pts_ns;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t layers;
    uint64_t usage;
    int acquire_fence_fd; /* Retained by caller; broker receives a duplicate. */
};

int advc_client_create_session(int fd, const struct advc_client_session_config *config,
                               uint32_t *session_id, uint32_t *detail);
/* Returns the exact tightly packed I420/NV12 frame size, or -1 with errno. */
int advc_client_encode_frame_size(const struct advc_client_session_config *config,
                                  size_t *frame_size);
int advc_client_queue_input(int fd, uint32_t session_id,
                            const struct advc_client_input *input, uint32_t *detail);
int advc_client_dequeue_output(int fd, uint32_t session_id,
                               struct advc_client_output *output, uint32_t *detail);
int advc_client_release_output(int fd, uint32_t session_id, uint64_t buffer_id,
                               uint32_t *detail);
int advc_client_transfer_ahb(int fd, uint32_t session_id, uint64_t buffer_id,
                            advc_receive_native_buffer_fn receive_buffer,
                            void *userdata, void **native_buffer, uint32_t *detail);
/*
 * Version 1.6: transfer authoritative PRIME metadata and owned dma-buf object
 * FDs for one dequeued AHB output. The acquire fence remains the one returned
 * by dequeue_output. Close the result with advc_dmabuf_descriptor_close().
 */
int advc_client_transfer_prime(
    int fd, uint32_t session_id, uint64_t buffer_id,
    struct advc_dmabuf_descriptor *descriptor, uint32_t *detail);
/*
 * Version 1.8 gateway operation: reserve an exportable LINEAR NV12 surface
 * before Android emits the matching PTS.  descriptor->buffer_id is the
 * cancellation token; release it with advc_client_release_output().
 */
int advc_client_reserve_linear(
    int fd, uint32_t session_id, uint64_t pts_ns, uint32_t width,
    uint32_t height, struct advc_dmabuf_descriptor *descriptor,
    uint32_t *detail);
int advc_client_submit_ahb(int fd, uint32_t session_id,
                           const struct advc_client_ahb_input *input,
                           advc_send_native_buffer_fn send_buffer,
                           void *userdata, int *release_fence_fd,
                           uint32_t *detail);
int advc_client_register_dmabuf(
    int fd, uint32_t session_id,
    const struct advc_dmabuf_descriptor *descriptor, uint32_t *detail);
int advc_client_unregister_dmabuf(int fd, uint32_t session_id,
                                  uint64_t buffer_id, uint32_t *detail);
int advc_client_queue_dmabuf(int fd, uint32_t session_id,
                             const struct advc_dmabuf_submission *submission,
                             uint32_t *detail);
/* Caller owns a returned release fence; -1 means completion was synchronous. */
int advc_client_complete_dmabuf(int fd, uint32_t session_id,
                                uint64_t buffer_id, int *release_fence_fd,
                                uint32_t *detail);
int advc_client_release_output_fenced(int fd, uint32_t session_id, uint64_t buffer_id,
                                      int release_fence_fd, uint32_t *detail);
int advc_client_flush(int fd, uint32_t session_id, uint32_t *detail);
int advc_client_close_session(int fd, uint32_t session_id, uint32_t *detail);
void advc_client_output_close(struct advc_client_output *output);

#ifdef __cplusplus
}
#endif

#endif
