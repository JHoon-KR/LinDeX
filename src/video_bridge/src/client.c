#define _GNU_SOURCE
#include "advc/client.h"
#include "advc/broker.h"

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

/*
 * Optional Firefox RDD handoff supplied by the tightly gated LinDeX preload
 * adapter.  A weak reference keeps every normal ADVC client unchanged.
 */
extern int lindex_firefox_rdd_take_broker_socket(const char *path)
    __attribute__((weak));
extern const char *lindex_firefox_rdd_broker_path(void)
    __attribute__((weak));
extern int lindex_firefox_rdd_debug_enabled(void) __attribute__((weak));
extern int lindex_firefox_rdd_recycle_broker_socket(int fd)
    __attribute__((weak));

typedef int (*firefox_take_socket_fn)(const char *path);
typedef const char *(*firefox_broker_path_fn)(void);
typedef int (*firefox_debug_enabled_fn)(void);
typedef int (*firefox_recycle_socket_fn)(int fd);

static void function_from_symbol(void *symbol, void *function,
                                 size_t function_size) {
    if (function_size == sizeof(symbol))
        memcpy(function, &symbol, function_size);
}

static firefox_take_socket_fn firefox_take_socket(void) {
    firefox_take_socket_fn function = lindex_firefox_rdd_take_broker_socket;
    if (function == NULL) {
        void *symbol = dlsym(RTLD_DEFAULT,
                             "lindex_firefox_rdd_take_broker_socket");
        function_from_symbol(symbol, &function, sizeof(function));
    }
    return function;
}

static firefox_broker_path_fn firefox_broker_path(void) {
    firefox_broker_path_fn function = lindex_firefox_rdd_broker_path;
    if (function == NULL) {
        void *symbol =
            dlsym(RTLD_DEFAULT, "lindex_firefox_rdd_broker_path");
        function_from_symbol(symbol, &function, sizeof(function));
    }
    return function;
}

static firefox_debug_enabled_fn firefox_debug_enabled(void) {
    firefox_debug_enabled_fn function = lindex_firefox_rdd_debug_enabled;
    if (function == NULL) {
        void *symbol =
            dlsym(RTLD_DEFAULT, "lindex_firefox_rdd_debug_enabled");
        function_from_symbol(symbol, &function, sizeof(function));
    }
    return function;
}

static firefox_recycle_socket_fn firefox_recycle_socket(void) {
    firefox_recycle_socket_fn function =
        lindex_firefox_rdd_recycle_broker_socket;
    if (function == NULL) {
        void *symbol = dlsym(RTLD_DEFAULT,
                             "lindex_firefox_rdd_recycle_broker_socket");
        function_from_symbol(symbol, &function, sizeof(function));
    }
    return function;
}

int advc_client_recycle_broker_socket(int fd) {
    firefox_recycle_socket_fn recycle = firefox_recycle_socket();
    if (fd < 0 || recycle == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return recycle(fd);
}

static uint32_t next_request_id(void) {
    static uint32_t value = 1;
    return value++;
}

static int parse_status_reply(struct advc_message *reply, const uint8_t *payload,
                              uint32_t *detail);
static int session_status_transaction(int fd, uint16_t opcode,
                                      uint32_t session_id,
                                      const uint8_t *request_payload,
                                      uint32_t request_payload_size,
                                      const int *request_fds,
                                      uint16_t request_fd_count,
                                      uint32_t *detail);

static int transaction_ex(int fd, uint16_t opcode, uint32_t session_id,
                          const uint8_t *request_payload, uint32_t request_payload_size,
                          const int *request_fds, uint16_t request_fd_count,
                          uint8_t *payload, size_t capacity,
                          struct advc_message *reply, int require_v11) {
    struct advc_message request;
    if (request_fd_count > ADVC_MAX_FDS ||
        (request_fd_count > 0 && request_fds == NULL)) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.header.version_major = ADVC_VERSION_MAJOR;
    request.header.version_minor = ADVC_VERSION_MINOR;
    request.header.message_type = ADVC_MSG_REQUEST;
    request.header.opcode = opcode;
    request.header.request_id = next_request_id();
    request.header.session_id = session_id;
    request.header.payload_size = request_payload_size;
    request.header.fd_count = request_fd_count;
    request.payload = (uint8_t *)request_payload;
    for (uint16_t i = 0; i < request_fd_count; ++i) {
        if (request_fds[i] < 0) {
            errno = EINVAL;
            return -1;
        }
        request.fds[i] = request_fds[i];
    }
    if (advc_send_message(fd, &request) < 0) {
        if (getenv("ADVC_VAAPI_TRACE") != NULL)
            fprintf(stderr,
                    "advc-client: send opcode=%u request=%u errno=%d (%s)\n",
                    opcode, request.header.request_id, errno,
                    strerror(errno));
        return -1;
    }

    memset(reply, 0, sizeof(*reply));
    reply->payload = payload;
    reply->payload_capacity = capacity;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) reply->fds[i] = -1;
    if (advc_receive_message(fd, reply) < 0) {
        if (getenv("ADVC_VAAPI_TRACE") != NULL)
            fprintf(stderr,
                    "advc-client: receive opcode=%u request=%u errno=%d (%s)\n",
                    opcode, request.header.request_id, errno,
                    strerror(errno));
        return -1;
    }
    if (reply->header.message_type != ADVC_MSG_REPLY || reply->header.opcode != opcode ||
        reply->header.request_id != request.header.request_id ||
        ((opcode == ADVC_OP_TRANSFER_AHB || opcode == ADVC_OP_QUEUE_AHB) ?
             (reply->header.flags != 0 &&
              reply->header.flags != ADVC_FLAG_AHB_FOLLOWS) :
             reply->header.flags != 0) ||
        (require_v11 && reply->header.version_minor != ADVC_VERSION_MINOR) ||
        (opcode != ADVC_OP_CREATE_SESSION && reply->header.session_id != session_id)) {
        advc_close_message_fds(reply);
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int advc_client_submit_ahb(int fd, uint32_t session_id,
                           const struct advc_client_ahb_input *input,
                           advc_send_native_buffer_fn send_buffer,
                           void *userdata, int *release_fence_fd,
                           uint32_t *detail) {
    uint8_t queue[ADVC_QUEUE_AHB_SIZE] = {0};
    uint8_t payload[ADVC_COMPLETE_AHB_SIZE];
    struct advc_message reply;
    int fence_fd;
    int status;
    if (session_id == 0 || input == NULL || input->native_buffer == NULL ||
        input->width == 0 || input->height == 0 || input->layers != 1 ||
        input->pts_ns > (uint64_t)INT64_MAX || input->acquire_fence_fd < -1 ||
        send_buffer == NULL || release_fence_fd == NULL) {
        errno = EINVAL;
        return -1;
    }
    *release_fence_fd = -1;
    advc_put_u64(queue + ADVC_QUEUE_AHB_PTS_NS_OFFSET, input->pts_ns);
    advc_put_u32(queue + ADVC_QUEUE_AHB_WIDTH_OFFSET, input->width);
    advc_put_u32(queue + ADVC_QUEUE_AHB_HEIGHT_OFFSET, input->height);
    advc_put_u32(queue + ADVC_QUEUE_AHB_FORMAT_OFFSET, input->format);
    advc_put_u32(queue + ADVC_QUEUE_AHB_LAYERS_OFFSET, input->layers);
    advc_put_u64(queue + ADVC_QUEUE_AHB_USAGE_OFFSET, input->usage);
    advc_put_u32(queue + ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET,
                 input->acquire_fence_fd >= 0 ? ADVC_FD_ACQUIRE_FENCE : ADVC_FD_NONE);
    fence_fd = input->acquire_fence_fd;
    if (transaction_ex(fd, ADVC_OP_QUEUE_AHB, session_id, queue, sizeof(queue),
                       fence_fd >= 0 ? &fence_fd : NULL, fence_fd >= 0 ? 1 : 0,
                       payload, sizeof(payload), &reply, 1) < 0)
        return -1;
    status = parse_status_reply(&reply, payload, detail);
    if (status != ADVC_STATUS_OK) return status;
    if (reply.header.flags != ADVC_FLAG_AHB_FOLLOWS ||
        send_buffer(fd, input->native_buffer, userdata) != 0) {
        (void)shutdown(fd, SHUT_RDWR);
        errno = EPROTO;
        return -1;
    }
    if (transaction_ex(fd, ADVC_OP_COMPLETE_AHB, session_id, NULL, 0, NULL, 0,
                       payload, sizeof(payload), &reply, 1) < 0) {
        (void)shutdown(fd, SHUT_RDWR);
        return -1;
    }
    if (reply.header.payload_size != ADVC_COMPLETE_AHB_SIZE ||
        reply.header.fd_count > 1) {
        advc_close_message_fds(&reply);
        (void)shutdown(fd, SHUT_RDWR);
        errno = EPROTO;
        return -1;
    }
    status = (int)advc_get_u32(payload + ADVC_COMPLETE_AHB_STATUS_OFFSET);
    if (status > ADVC_STATUS_INTERNAL) {
        advc_close_message_fds(&reply);
        (void)shutdown(fd, SHUT_RDWR);
        errno = EPROTO;
        return -1;
    }
    if (detail != NULL)
        *detail = advc_get_u32(payload + ADVC_COMPLETE_AHB_DETAIL_OFFSET);
    {
        uint32_t role = advc_get_u32(payload + ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET);
        if ((role == ADVC_FD_NONE && reply.header.fd_count != 0) ||
            (role == ADVC_FD_RELEASE_FENCE && reply.header.fd_count != 1) ||
            (role != ADVC_FD_NONE && role != ADVC_FD_RELEASE_FENCE) ||
            (status != ADVC_STATUS_OK && reply.header.fd_count != 0)) {
            advc_close_message_fds(&reply);
            (void)shutdown(fd, SHUT_RDWR);
            errno = EPROTO;
            return -1;
        }
    }
    if (reply.header.fd_count == 1) {
        *release_fence_fd = reply.fds[0];
        reply.fds[0] = -1;
    }
    advc_close_message_fds(&reply);
    return status;
}

int advc_client_register_dmabuf(
    int fd, uint32_t session_id,
    const struct advc_dmabuf_descriptor *descriptor, uint32_t *detail) {
    uint8_t payload[ADVC_REGISTER_DMABUF_SIZE] = {0};
    int fds[ADVC_MAX_DMABUF_OBJECTS];
    uint16_t fd_count = 0;
    if (session_id == 0 || descriptor == NULL ||
        advc_dmabuf_registration_encode(payload, descriptor, fds,
                                         &fd_count) < 0)
        return -1;
    return session_status_transaction(
        fd, ADVC_OP_REGISTER_DMABUF, session_id, payload, sizeof(payload), fds,
        fd_count, detail);
}

int advc_client_transfer_prime(
    int fd, uint32_t session_id, uint64_t buffer_id,
    struct advc_dmabuf_descriptor *descriptor, uint32_t *detail) {
    uint8_t request[ADVC_TRANSFER_PRIME_SIZE] = {0};
    uint8_t payload[ADVC_TRANSFER_PRIME_REPLY_SIZE];
    struct advc_message reply;
    uint32_t status;
    uint32_t object_count;

    if (session_id == 0 || buffer_id == 0 || descriptor == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    advc_put_u64(request + ADVC_TRANSFER_PRIME_BUFFER_ID_OFFSET, buffer_id);
    if (transaction_ex(fd, ADVC_OP_TRANSFER_PRIME, session_id, request,
                       sizeof(request), NULL, 0, payload, sizeof(payload),
                       &reply, 1) < 0)
        return -1;
    if (reply.header.payload_size == ADVC_STATUS_SIZE)
        return parse_status_reply(&reply, payload, detail);
    if (reply.header.payload_size != ADVC_TRANSFER_PRIME_REPLY_SIZE ||
        reply.header.flags != 0) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    status = advc_get_u32(payload + ADVC_STATUS_CODE_OFFSET);
    if (status != ADVC_STATUS_OK ||
        advc_get_u32(payload + ADVC_STATUS_DETAIL_OFFSET) != 0) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    object_count = advc_get_u32(
        payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET +
        ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET);
    if (object_count == 0 || object_count > ADVC_MAX_DMABUF_OBJECTS ||
        reply.header.fd_count != object_count ||
        advc_dmabuf_registration_decode(
            payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
            ADVC_REGISTER_DMABUF_SIZE, reply.fds, reply.header.fd_count,
            descriptor) < 0 ||
        descriptor->buffer_id != buffer_id) {
        advc_close_message_fds(&reply);
        memset(descriptor, 0, sizeof(*descriptor));
        for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
            descriptor->objects[i].fd = -1;
        errno = EPROTO;
        return -1;
    }
    for (uint32_t i = 0; i < descriptor->object_count; ++i)
        reply.fds[i] = -1;
    advc_close_message_fds(&reply);
    if (detail != NULL) *detail = 0;
    return ADVC_STATUS_OK;
}

int advc_client_reserve_linear(
    int fd, uint32_t session_id, uint64_t pts_ns, uint32_t width,
    uint32_t height, struct advc_dmabuf_descriptor *descriptor,
    uint32_t *detail) {
    uint8_t request[ADVC_RESERVE_LINEAR_SIZE] = {0};
    uint8_t payload[ADVC_RESERVE_LINEAR_REPLY_SIZE];
    struct advc_message reply;
    uint32_t status;
    uint32_t object_count;

    if (session_id == 0 || pts_ns > (uint64_t)INT64_MAX || width == 0 ||
        height == 0 || (width & 1u) != 0 || (height & 1u) != 0 ||
        descriptor == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    advc_put_u64(request + ADVC_RESERVE_LINEAR_PTS_NS_OFFSET, pts_ns);
    advc_put_u32(request + ADVC_RESERVE_LINEAR_WIDTH_OFFSET, width);
    advc_put_u32(request + ADVC_RESERVE_LINEAR_HEIGHT_OFFSET, height);
    advc_put_u32(request + ADVC_RESERVE_LINEAR_FOURCC_OFFSET,
                 UINT32_C(0x3231564e));
    if (transaction_ex(fd, ADVC_OP_RESERVE_LINEAR, session_id, request,
                       sizeof(request), NULL, 0, payload, sizeof(payload),
                       &reply, 1) < 0)
        return -1;
    if (reply.header.payload_size == ADVC_STATUS_SIZE)
        return parse_status_reply(&reply, payload, detail);
    if (reply.header.payload_size != ADVC_RESERVE_LINEAR_REPLY_SIZE ||
        reply.header.flags != 0) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    status = advc_get_u32(payload + ADVC_STATUS_CODE_OFFSET);
    object_count = advc_get_u32(
        payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET +
        ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET);
    if (status != ADVC_STATUS_OK ||
        advc_get_u32(payload + ADVC_STATUS_DETAIL_OFFSET) != 0 ||
        object_count == 0 || object_count > ADVC_MAX_DMABUF_OBJECTS ||
        reply.header.fd_count != object_count ||
        advc_dmabuf_registration_decode(
            payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
            ADVC_REGISTER_DMABUF_SIZE, reply.fds, reply.header.fd_count,
            descriptor) < 0 || descriptor->buffer_id == 0 ||
        descriptor->width != width || descriptor->height != height ||
        descriptor->drm_fourcc != UINT32_C(0x3231564e) ||
        descriptor->drm_modifier != 0) {
        advc_close_message_fds(&reply);
        advc_dmabuf_descriptor_close(descriptor);
        errno = EPROTO;
        return -1;
    }
    for (uint32_t i = 0; i < descriptor->object_count; ++i)
        reply.fds[i] = -1;
    advc_close_message_fds(&reply);
    if (detail != NULL) *detail = 0;
    return ADVC_STATUS_OK;
}

int advc_client_unregister_dmabuf(int fd, uint32_t session_id,
                                  uint64_t buffer_id, uint32_t *detail) {
    uint8_t payload[ADVC_UNREGISTER_DMABUF_SIZE] = {0};
    if (buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    advc_put_u64(payload + ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET, buffer_id);
    return session_status_transaction(
        fd, ADVC_OP_UNREGISTER_DMABUF, session_id, payload, sizeof(payload),
        NULL, 0, detail);
}

int advc_client_queue_dmabuf(int fd, uint32_t session_id,
                             const struct advc_dmabuf_submission *submission,
                             uint32_t *detail) {
    uint8_t payload[ADVC_QUEUE_DMABUF_SIZE] = {0};
    const int *fds = NULL;
    uint16_t fd_count = 0;
    if (submission == NULL || submission->buffer_id == 0 ||
        submission->pts_ns > (uint64_t)INT64_MAX ||
        submission->acquire_fence_fd < -1 ||
        (submission->acquire_fence_fd >= 0 &&
         advc_dmabuf_sync_file_validate(submission->acquire_fence_fd) < 0)) {
        errno = EINVAL;
        return -1;
    }
    advc_put_u64(payload + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET,
                 submission->buffer_id);
    advc_put_u64(payload + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET,
                 submission->pts_ns);
    advc_put_u32(payload + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET,
                 submission->acquire_fence_fd >= 0 ?
                 ADVC_FD_ACQUIRE_FENCE : ADVC_FD_NONE);
    if (submission->acquire_fence_fd >= 0) {
        fds = &submission->acquire_fence_fd;
        fd_count = 1;
    }
    return session_status_transaction(
        fd, ADVC_OP_QUEUE_DMABUF, session_id, payload, sizeof(payload), fds,
        fd_count, detail);
}

int advc_client_complete_dmabuf(int fd, uint32_t session_id,
                                uint64_t buffer_id, int *release_fence_fd,
                                uint32_t *detail) {
    uint8_t request[ADVC_COMPLETE_DMABUF_REQUEST_SIZE] = {0};
    uint8_t payload[ADVC_COMPLETE_DMABUF_SIZE];
    struct advc_message reply;
    int status;
    if (session_id == 0 || buffer_id == 0 || release_fence_fd == NULL) {
        errno = EINVAL;
        return -1;
    }
    *release_fence_fd = -1;
    advc_put_u64(request + ADVC_COMPLETE_DMABUF_REQUEST_BUFFER_ID_OFFSET,
                 buffer_id);
    if (transaction_ex(fd, ADVC_OP_COMPLETE_DMABUF, session_id, request,
                       sizeof(request), NULL, 0, payload, sizeof(payload),
                       &reply, 1) < 0)
        return -1;
    if (reply.header.payload_size == ADVC_STATUS_SIZE)
        return parse_status_reply(&reply, payload, detail);
    if (advc_dmabuf_completion_validate(payload, reply.header.payload_size,
                                         reply.fds,
                                         reply.header.fd_count) < 0 ||
        advc_get_u64(payload + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET) !=
            buffer_id) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    status = (int)advc_get_u32(payload + ADVC_COMPLETE_DMABUF_STATUS_OFFSET);
    if (detail != NULL)
        *detail = advc_get_u32(payload + ADVC_COMPLETE_DMABUF_DETAIL_OFFSET);
    if (reply.header.fd_count == 1) {
        *release_fence_fd = reply.fds[0];
        reply.fds[0] = -1;
    }
    advc_close_message_fds(&reply);
    return status;
}

static int transaction(int fd, uint16_t opcode, const uint8_t *request_payload,
                       uint32_t request_payload_size, uint8_t *payload, size_t capacity,
                       struct advc_message *reply) {
    return transaction_ex(fd, opcode, 0, request_payload, request_payload_size,
                          NULL, 0, payload, capacity, reply, 0);
}

static int parse_status_reply(struct advc_message *reply, const uint8_t *payload,
                              uint32_t *detail) {
    uint32_t status;
    if (reply->header.payload_size != ADVC_STATUS_SIZE || reply->header.fd_count != 0) {
        advc_close_message_fds(reply);
        errno = EPROTO;
        return -1;
    }
    status = advc_get_u32(payload + ADVC_STATUS_CODE_OFFSET);
    if (status > ADVC_STATUS_INTERNAL) {
        errno = EPROTO;
        return -1;
    }
    if (detail != NULL) *detail = advc_get_u32(payload + ADVC_STATUS_DETAIL_OFFSET);
    return (int)status;
}

static int session_status_transaction(int fd, uint16_t opcode, uint32_t session_id,
                                      const uint8_t *request_payload,
                                      uint32_t request_payload_size,
                                      const int *request_fds, uint16_t request_fd_count,
                                      uint32_t *detail) {
    uint8_t payload[ADVC_STATUS_SIZE];
    struct advc_message reply;
    if (session_id == 0) {
        errno = EINVAL;
        return -1;
    }
    if (transaction_ex(fd, opcode, session_id, request_payload, request_payload_size,
                       request_fds, request_fd_count, payload, sizeof(payload),
                       &reply, 1) < 0)
        return -1;
    return parse_status_reply(&reply, payload, detail);
}

static int64_t client_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 ||
        now.tv_sec > (time_t)(INT64_MAX / 1000))
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int restore_blocking_socket(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int remaining_connect_ms(int64_t deadline) {
    int64_t now = client_monotonic_ms();
    int64_t remaining;
    if (now < 0) return -1;
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }
    remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static int wait_for_nonblocking_connect(int fd, int64_t deadline) {
    struct pollfd item = {.fd = fd, .events = POLLOUT};
    for (;;) {
        int timeout = remaining_connect_ms(deadline);
        int result;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (timeout < 0) return -1;
        result = poll(&item, 1, timeout);
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) < 0)
            return -1;
        if (socket_error != 0) {
            errno = socket_error;
            return -1;
        }
        return 0;
    }
}

static int wait_before_connect_retry(int64_t deadline) {
    for (;;) {
        int remaining = remaining_connect_ms(deadline);
        int delay;
        int result;
        if (remaining < 0) return -1;
        delay = remaining > 10 ? 10 : remaining;
        result = poll(NULL, 0, delay);
        if (result == 0) return 0;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
    }
}

static int connect_with_deadline(const struct sockaddr_un *address,
                                 int64_t deadline) {
    for (;;) {
        int fd = socket(AF_UNIX,
                        SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        int saved;
        if (fd < 0) return -1;
        if (connect(fd, (const struct sockaddr *)address,
                    sizeof(*address)) == 0) {
            if (restore_blocking_socket(fd) == 0) return fd;
            saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        saved = errno;
        if (saved == EINPROGRESS || saved == EALREADY) {
            if (wait_for_nonblocking_connect(fd, deadline) == 0) {
                if (restore_blocking_socket(fd) == 0) return fd;
                saved = errno;
                close(fd);
                errno = saved;
                return -1;
            }
            saved = errno;
        }
        close(fd);
        if (saved != EAGAIN) {
            errno = saved;
            return -1;
        }
        if (wait_before_connect_retry(deadline) < 0) return -1;
    }
}

static int advc_client_connect_internal(const char *path, int bounded,
                                        uint32_t timeout_ms) {
    struct sockaddr_un address;
    const char *effective_path = path;
    firefox_take_socket_fn take_socket = firefox_take_socket();
    firefox_broker_path_fn get_broker_path = firefox_broker_path();
    firefox_debug_enabled_fn get_debug_enabled = firefox_debug_enabled();
    int64_t deadline = 0;
    int fd;
    int trace = getenv("ADVC_VAAPI_TRACE") != NULL ||
                access("/run/android-drm/lindex-vaapi-trace", F_OK) == 0 ||
                (get_debug_enabled != NULL && get_debug_enabled());
    if (path == NULL || strlen(path) >= sizeof(address.sun_path) ||
        (bounded && timeout_ms == 0)) {
        errno = EINVAL;
        return -1;
    }
    if (bounded) {
        int64_t now = client_monotonic_ms();
        if (now < 0 || now > INT64_MAX - timeout_ms) {
            errno = EOVERFLOW;
            return -1;
        }
        deadline = now + timeout_ms;
    }
    if (get_broker_path != NULL) {
        const char *prepared_path = get_broker_path();
        if (prepared_path != NULL && prepared_path[0] != '\0')
            effective_path = prepared_path;
    }
    if (strlen(effective_path) >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    if (trace)
        fprintf(stderr,
                "advc-client: connect path=%s effective=%s handoff=%s\n",
                path, effective_path,
                take_socket != NULL ? "present" : "missing");
    if (take_socket != NULL) {
        fd = take_socket(effective_path);
        if (fd >= 0) {
            if (trace)
                fprintf(stderr, "advc-client: handoff fd=%d\n", fd);
            return fd;
        }
        if (trace)
            fprintf(stderr, "advc-client: handoff failed errno=%d (%s)\n",
                    errno, strerror(errno));
        if (errno != ENOSYS) return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, effective_path, sizeof(address.sun_path) - 1);
    if (bounded) {
        fd = connect_with_deadline(&address, deadline);
    } else {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&address,
                               sizeof(address)) < 0) {
            int saved = errno;
            close(fd);
            fd = -1;
            errno = saved;
        }
    }
    if (fd < 0) {
        int saved = errno;
        if (trace)
            fprintf(stderr, "advc-client: connect failed errno=%d (%s)\n",
                    saved, strerror(saved));
        errno = saved;
        return -1;
    }
    return fd;
}

int advc_client_connect(const char *path) {
    return advc_client_connect_internal(path, 0, 0);
}

int advc_client_connect_bounded(const char *path, uint32_t timeout_ms) {
    return advc_client_connect_internal(path, 1, timeout_ms);
}

int advc_client_hello(int fd, uint64_t client_features, uint64_t *features,
                      uint32_t *max_payload) {
    uint8_t request_payload[ADVC_HELLO_SIZE] = {0};
    uint8_t payload[ADVC_HELLO_SIZE];
    struct advc_message reply;
    advc_put_u64(request_payload + ADVC_HELLO_FEATURES_OFFSET, client_features);
    advc_put_u32(request_payload + ADVC_HELLO_MAX_PAYLOAD_OFFSET, ADVC_MAX_PAYLOAD);
    if (transaction(fd, ADVC_OP_HELLO, request_payload, sizeof(request_payload),
                    payload, sizeof(payload), &reply) < 0) return -1;
    if (reply.header.payload_size != ADVC_HELLO_SIZE || reply.header.fd_count != 0) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    if (features != NULL) *features = advc_get_u64(payload + ADVC_HELLO_FEATURES_OFFSET);
    if (max_payload != NULL) *max_payload = advc_get_u32(payload + ADVC_HELLO_MAX_PAYLOAD_OFFSET);
    return 0;
}

int advc_client_query_capabilities(int fd, struct advc_capability_set *caps) {
    uint8_t payload[ADVC_CAPS_PREFIX_SIZE + ADVC_MAX_CAPABILITIES * ADVC_CAPS_ENTRY_SIZE];
    struct advc_message reply;
    uint32_t count;
    if (caps == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (transaction(fd, ADVC_OP_QUERY_CAPABILITIES, NULL, 0,
                    payload, sizeof(payload), &reply) < 0) return -1;
    if (reply.header.fd_count != 0) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    if (reply.header.payload_size == ADVC_STATUS_SIZE &&
        advc_get_u32(payload + ADVC_STATUS_CODE_OFFSET) != ADVC_STATUS_OK) {
        int status = parse_status_reply(&reply, payload, NULL);
        if (status < 0) return -1;
        errno = ENOTSUP;
        return -1;
    }
    if (reply.header.payload_size < ADVC_CAPS_PREFIX_SIZE) {
        errno = EPROTO;
        return -1;
    }
    count = advc_get_u32(payload + 4);
    if (count > ADVC_MAX_CAPABILITIES ||
        reply.header.payload_size != ADVC_CAPS_PREFIX_SIZE + count * ADVC_CAPS_ENTRY_SIZE) {
        errno = EPROTO;
        return -1;
    }
    memset(caps, 0, sizeof(*caps));
    caps->count = count;
    caps->api_level = advc_get_u32(payload + 8);
    caps->transport_features = advc_get_u64(payload + 16);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = payload + ADVC_CAPS_PREFIX_SIZE + i * ADVC_CAPS_ENTRY_SIZE;
        struct advc_codec_capability *c = &caps->codecs[i];
        memcpy(c->mime, entry + ADVC_CAPS_ENTRY_MIME_OFFSET, ADVC_MAX_MIME);
        memcpy(c->codec_name, entry + ADVC_CAPS_ENTRY_NAME_OFFSET, ADVC_MAX_CODEC_NAME);
        c->mime[ADVC_MAX_MIME - 1] = '\0';
        c->codec_name[ADVC_MAX_CODEC_NAME - 1] = '\0';
        c->direction = entry[ADVC_CAPS_ENTRY_DIRECTION_OFFSET];
        c->acceleration = entry[ADVC_CAPS_ENTRY_ACCELERATION_OFFSET];
        c->low_latency = entry[ADVC_CAPS_ENTRY_LOW_LATENCY_OFFSET];
        c->secure_playback = entry[ADVC_CAPS_ENTRY_SECURE_OFFSET];
        c->max_width = advc_get_u32(entry + ADVC_CAPS_ENTRY_MAX_WIDTH_OFFSET);
        c->max_height = advc_get_u32(entry + ADVC_CAPS_ENTRY_MAX_HEIGHT_OFFSET);
        c->max_fps_milli = advc_get_u32(entry + ADVC_CAPS_ENTRY_MAX_FPS_MILLI_OFFSET);
        c->flags = advc_get_u32(entry + ADVC_CAPS_ENTRY_FLAGS_OFFSET);
    }
    return 0;
}

int advc_client_create_session(int fd, const struct advc_client_session_config *config,
                               uint32_t *session_id, uint32_t *detail) {
    uint8_t request[ADVC_CREATE_SIZE] = {0};
    uint8_t payload[ADVC_STATUS_SIZE];
    struct advc_message reply;
    size_t mime_length;
    int status;
    if (config == NULL || session_id == NULL || config->mime == NULL) {
        errno = EINVAL;
        return -1;
    }
    mime_length = strnlen(config->mime, ADVC_MAX_MIME);
    if (mime_length == 0 || mime_length >= ADVC_MAX_MIME ||
        strncmp(config->mime, "video/", 6) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (config->direction == ADVC_DIRECTION_DECODE) {
        if (config->color_format != 0 ||
            config->encode_profile != ADVC_ENCODE_PROFILE_NONE ||
            (config->transport != 0 && config->transport != ADVC_TRANSPORT_BYTES &&
             config->transport != ADVC_TRANSPORT_AHARDWAREBUFFER)) {
            errno = EINVAL;
            return -1;
        }
    } else if (config->direction == ADVC_DIRECTION_ENCODE) {
        uint32_t transport = config->transport == 0 ? ADVC_TRANSPORT_BYTES :
                                                      config->transport;
        if ((strcmp(config->mime, "video/avc") == 0 &&
             config->encode_profile !=
                 ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE &&
             config->encode_profile != ADVC_ENCODE_PROFILE_H264_MAIN &&
             config->encode_profile != ADVC_ENCODE_PROFILE_H264_HIGH) ||
            (strcmp(config->mime, "video/hevc") == 0 &&
             config->encode_profile != ADVC_ENCODE_PROFILE_HEVC_MAIN) ||
            (strcmp(config->mime, "video/avc") != 0 &&
             strcmp(config->mime, "video/hevc") != 0)) {
            errno = ENOTSUP;
            return -1;
        }
        if (transport != ADVC_TRANSPORT_BYTES &&
            transport != ADVC_TRANSPORT_BROKER_EGL_SURFACE &&
            transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE &&
            transport != ADVC_TRANSPORT_DMABUF) {
            errno = ENOTSUP;
            return -1;
        }
        if (transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
            transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
            transport == ADVC_TRANSPORT_DMABUF) {
            if (config->color_format != 0 || config->flags != 0 ||
                config->width < 16 || config->width > 8192 ||
                config->height < 16 || config->height > 8192 ||
                (config->width & 1u) != 0 || (config->height & 1u) != 0 ||
                config->bitrate == 0 ||
                config->bitrate > ADVC_MAX_ENCODE_BITRATE ||
                config->framerate_milli < 1000 ||
                config->framerate_milli > 240000 ||
                (strcmp(config->mime, "video/avc") != 0 &&
                 strcmp(config->mime, "video/hevc") != 0)) {
                errno = EINVAL;
                return -1;
            }
        } else {
            size_t frame_size;
            if (advc_client_encode_frame_size(config, &frame_size) < 0) return -1;
            (void)frame_size;
        }
    } else {
        errno = EINVAL;
        return -1;
    }
    advc_put_u32(request + ADVC_CREATE_DIRECTION_OFFSET, config->direction);
    advc_put_u32(request + ADVC_CREATE_WIDTH_OFFSET, config->width);
    advc_put_u32(request + ADVC_CREATE_HEIGHT_OFFSET, config->height);
    advc_put_u32(request + ADVC_CREATE_BITRATE_OFFSET, config->bitrate);
    advc_put_u32(request + ADVC_CREATE_FRAMERATE_MILLI_OFFSET,
                 config->framerate_milli);
    advc_put_u32(request + ADVC_CREATE_FLAGS_OFFSET, config->flags);
    memcpy(request + ADVC_CREATE_MIME_OFFSET, config->mime, mime_length);
    advc_put_u32(request + ADVC_CREATE_COLOR_FORMAT_OFFSET, config->color_format);
    advc_put_u32(request + ADVC_CREATE_TRANSPORT_OFFSET,
                 config->transport == 0 ? ADVC_TRANSPORT_BYTES : config->transport);
    advc_put_u32(request + ADVC_CREATE_ENCODE_PROFILE_OFFSET,
                 config->encode_profile);
    if (transaction_ex(fd, ADVC_OP_CREATE_SESSION, 0, request, sizeof(request),
                       NULL, 0, payload, sizeof(payload), &reply, 1) < 0)
        return -1;
    status = parse_status_reply(&reply, payload, detail);
    if (status < 0) return -1;
    if (status == ADVC_STATUS_OK) {
        if (reply.header.session_id == 0) {
            errno = EPROTO;
            return -1;
        }
        *session_id = reply.header.session_id;
    } else if (reply.header.session_id != 0) {
        errno = EPROTO;
        return -1;
    }
    return status;
}

int advc_client_encode_frame_size(const struct advc_client_session_config *config,
                                  size_t *frame_size) {
    uint64_t pixels;
    uint64_t bytes;
    if (config == NULL || frame_size == NULL || config->mime == NULL ||
        config->direction != ADVC_DIRECTION_ENCODE ||
        (strcmp(config->mime, "video/avc") != 0 &&
         strcmp(config->mime, "video/hevc") != 0) ||
        config->flags != 0 || config->width < 16 || config->width > 8192 ||
        config->height < 16 || config->height > 8192 ||
        (config->width & 1u) != 0 || (config->height & 1u) != 0 ||
        config->bitrate == 0 || config->bitrate > ADVC_MAX_ENCODE_BITRATE ||
        config->framerate_milli < 1000 || config->framerate_milli > 240000 ||
        (config->color_format != ADVC_COLOR_FORMAT_YUV420_PLANAR &&
         config->color_format != ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR)) {
        errno = EINVAL;
        return -1;
    }
    pixels = (uint64_t)config->width * (uint64_t)config->height;
    bytes = pixels + pixels / 2u;
    if (bytes > ADVC_MAX_INPUT_BYTES || bytes > SIZE_MAX) {
        errno = E2BIG;
        return -1;
    }
    *frame_size = (size_t)bytes;
    return 0;
}

int advc_client_queue_input(int fd, uint32_t session_id,
                            const struct advc_client_input *input, uint32_t *detail) {
    uint8_t *payload;
    uint32_t payload_size;
    int request_fd;
    int status;
    const int *request_fds = NULL;
    uint16_t request_fd_count = 0;
    if (input == NULL || input->size > ADVC_MAX_INPUT_BYTES) {
        errno = EINVAL;
        return -1;
    }
    if ((input->flags & ~(ADVC_FLAG_END_OF_STREAM | ADVC_FLAG_KEY_FRAME |
                          ADVC_FLAG_CODEC_CONFIG)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (input->data_fd < 0) {
        if ((input->size > 0 && input->data == NULL) || input->data_offset != 0 ||
            input->size > ADVC_MAX_PAYLOAD - ADVC_QUEUE_INPUT_SIZE) {
            errno = EINVAL;
            return -1;
        }
        payload_size = ADVC_QUEUE_INPUT_SIZE + (uint32_t)input->size;
    } else {
        struct stat statbuf;
        int seals = fcntl(input->data_fd, F_GET_SEALS);
        int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
        if (input->data != NULL || seals < 0 || (seals & required) != required ||
            fstat(input->data_fd, &statbuf) < 0 || !S_ISREG(statbuf.st_mode) ||
            statbuf.st_size < 0 || input->data_offset > (uint64_t)statbuf.st_size ||
            (uint64_t)input->size > (uint64_t)statbuf.st_size - input->data_offset) {
            errno = EINVAL;
            return -1;
        }
        payload_size = ADVC_QUEUE_INPUT_SIZE;
        request_fd = input->data_fd;
        request_fds = &request_fd;
        request_fd_count = 1;
    }
    payload = (uint8_t *)calloc(1, payload_size > 0 ? payload_size : 1);
    if (payload == NULL) return -1;
    advc_put_u64(payload + ADVC_QUEUE_INPUT_BUFFER_ID_OFFSET, input->buffer_id);
    advc_put_u64(payload + ADVC_QUEUE_INPUT_PTS_NS_OFFSET, input->pts_ns);
    advc_put_u64(payload + ADVC_QUEUE_INPUT_DATA_OFFSET,
                 input->data_fd < 0 ? ADVC_QUEUE_INPUT_SIZE : input->data_offset);
    advc_put_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, input->size);
    advc_put_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET, input->flags);
    advc_put_u32(payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET,
                 input->data_fd < 0 ? ADVC_FD_NONE : ADVC_FD_INPUT_DATA);
    if (input->data_fd < 0 && input->size > 0)
        memcpy(payload + ADVC_QUEUE_INPUT_SIZE, input->data, input->size);
    status = session_status_transaction(fd, ADVC_OP_QUEUE_INPUT, session_id,
                                        payload, payload_size, request_fds,
                                        request_fd_count, detail);
    free(payload);
    return status;
}

int advc_client_dequeue_output(int fd, uint32_t session_id,
                               struct advc_client_output *output, uint32_t *detail) {
    uint8_t payload[ADVC_OUTPUT_BYTES_SIZE];
    struct advc_message reply;
    struct stat statbuf;
    uint64_t output_size;
    uint32_t flags;
    int seals;
    int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (session_id == 0 || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    output->data_fd = -1;
    output->acquire_fence_fd = -1;
    if (transaction_ex(fd, ADVC_OP_DEQUEUE_OUTPUT, session_id, NULL, 0, NULL, 0,
                       payload, sizeof(payload), &reply, 1) < 0)
        return -1;
    if (reply.header.payload_size == ADVC_STATUS_SIZE) {
        return parse_status_reply(&reply, payload, detail);
    }
    output->transport = advc_get_u32(payload + ADVC_OUTPUT_TRANSPORT_OFFSET);
    if ((output->transport == ADVC_TRANSPORT_BYTES &&
         (reply.header.payload_size != ADVC_OUTPUT_BYTES_SIZE || reply.header.fd_count != 1)) ||
        (output->transport == ADVC_TRANSPORT_AHARDWAREBUFFER &&
         (reply.header.payload_size != ADVC_OUTPUT_READY_SIZE || reply.header.fd_count > 1)) ||
        (output->transport != ADVC_TRANSPORT_BYTES &&
         output->transport != ADVC_TRANSPORT_AHARDWAREBUFFER) ||
        advc_get_u32(payload + ADVC_OUTPUT_PLANE_COUNT_OFFSET) != 0 ||
        (advc_get_u32(payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET) != ADVC_FD_NONE &&
         advc_get_u32(payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET) !=
             ADVC_FD_ACQUIRE_FENCE) ||
        ((advc_get_u32(payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET) ==
              ADVC_FD_ACQUIRE_FENCE) != (reply.header.fd_count == 1 &&
                  output->transport == ADVC_TRANSPORT_AHARDWAREBUFFER)) ||
        advc_get_u32(payload + ADVC_OUTPUT_DRM_FOURCC_OFFSET) != 0 ||
        advc_get_u64(payload + ADVC_OUTPUT_DRM_MODIFIER_OFFSET) != 0 ||
        (output->transport == ADVC_TRANSPORT_BYTES &&
         (advc_get_u32(payload + ADVC_OUTPUT_LAYERS_OFFSET) != 0 ||
          advc_get_u64(payload + ADVC_OUTPUT_USAGE_OFFSET) != 0)) ||
        (output->transport == ADVC_TRANSPORT_BYTES && advc_get_u32(payload + 132) != 0)) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    output_size = advc_get_u64(payload + ADVC_OUTPUT_SIZE_OFFSET);
    flags = advc_get_u32(payload + ADVC_OUTPUT_FLAGS_OFFSET);
    seals = output->transport == ADVC_TRANSPORT_BYTES ?
            fcntl(reply.fds[0], F_GET_SEALS) : required;
    if (advc_get_u64(payload + ADVC_OUTPUT_BUFFER_ID_OFFSET) == 0 ||
        output_size > ADVC_MAX_OUTPUT_BYTES ||
        (flags & ~(ADVC_FLAG_END_OF_STREAM | ADVC_FLAG_KEY_FRAME |
                   ADVC_FLAG_CODEC_CONFIG)) != 0 ||
        seals < 0 || (seals & required) != required ||
        (output->transport == ADVC_TRANSPORT_BYTES &&
         (fstat(reply.fds[0], &statbuf) < 0 || !S_ISREG(statbuf.st_mode) ||
          statbuf.st_size < 0 || output_size != (uint64_t)statbuf.st_size)) ||
        (output->transport == ADVC_TRANSPORT_AHARDWAREBUFFER && output_size != 0)) {
        advc_close_message_fds(&reply);
        errno = EPROTO;
        return -1;
    }
    output->buffer_id = advc_get_u64(payload + ADVC_OUTPUT_BUFFER_ID_OFFSET);
    output->pts_ns = advc_get_u64(payload + ADVC_OUTPUT_PTS_NS_OFFSET);
    output->size = output_size;
    output->flags = flags;
    output->width = advc_get_u32(payload + ADVC_OUTPUT_WIDTH_OFFSET);
    output->height = advc_get_u32(payload + ADVC_OUTPUT_HEIGHT_OFFSET);
    output->android_format = advc_get_u32(payload + ADVC_OUTPUT_ANDROID_FORMAT_OFFSET);
    output->stride = advc_get_u32(payload + ADVC_OUTPUT_STRIDE_OFFSET);
    output->layers = advc_get_u32(payload + ADVC_OUTPUT_LAYERS_OFFSET);
    output->usage = advc_get_u64(payload + ADVC_OUTPUT_USAGE_OFFSET);
    if (output->transport == ADVC_TRANSPORT_BYTES) {
        output->slice_height =
            advc_get_u32(payload + ADVC_OUTPUT_SLICE_HEIGHT_OFFSET);
        output->crop_left = advc_get_u32(payload + ADVC_OUTPUT_CROP_LEFT_OFFSET);
        output->crop_top = advc_get_u32(payload + ADVC_OUTPUT_CROP_TOP_OFFSET);
        output->crop_right = advc_get_u32(payload + ADVC_OUTPUT_CROP_RIGHT_OFFSET);
        output->crop_bottom = advc_get_u32(payload + ADVC_OUTPUT_CROP_BOTTOM_OFFSET);
    } else {
        /*
         * The 1.2 AHB payload ends at byte 112; crop fields belong to the
         * byte-transport extension. Never read the unreceived stack tail.
         * Until a versioned AHB crop extension exists, the transferred AHB
         * allocation is explicitly defined as a full-allocation frame.
         */
        output->slice_height = output->height;
        output->crop_right = output->width > 0 ? output->width - 1 : 0;
        output->crop_bottom = output->height > 0 ? output->height - 1 : 0;
    }
    if (output->transport == ADVC_TRANSPORT_BYTES) output->data_fd = reply.fds[0];
    else if (reply.header.fd_count == 1) output->acquire_fence_fd = reply.fds[0];
    if (reply.header.fd_count == 1) reply.fds[0] = -1;
    reply.header.fd_count = 0;
    if (detail != NULL) *detail = 0;
    return ADVC_STATUS_OK;
}

int advc_client_release_output(int fd, uint32_t session_id, uint64_t buffer_id,
                               uint32_t *detail) {
    return advc_client_release_output_fenced(fd, session_id, buffer_id, -1, detail);
}

int advc_client_release_output_fenced(int fd, uint32_t session_id, uint64_t buffer_id,
                                      int release_fence_fd, uint32_t *detail) {
    uint8_t payload[ADVC_RELEASE_OUTPUT_SIZE] = {0};
    const int *fds = NULL;
    uint16_t fd_count = 0;
    if (buffer_id == 0) {
        errno = EINVAL;
        return -1;
    }
    advc_put_u64(payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET, buffer_id);
    advc_put_u32(payload + ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET,
                 release_fence_fd >= 0 ? ADVC_FD_RELEASE_FENCE : ADVC_FD_NONE);
    if (release_fence_fd >= 0) {
        fds = &release_fence_fd;
        fd_count = 1;
    }
    return session_status_transaction(fd, ADVC_OP_RELEASE_OUTPUT, session_id,
                                      payload, sizeof(payload), fds, fd_count, detail);
}

int advc_client_transfer_ahb(int fd, uint32_t session_id, uint64_t buffer_id,
                            advc_receive_native_buffer_fn receive_buffer,
                            void *userdata, void **native_buffer, uint32_t *detail) {
    uint8_t request[ADVC_TRANSFER_AHB_SIZE] = {0};
    uint8_t payload[ADVC_STATUS_SIZE];
    struct advc_message reply;
    int status;
    if (session_id == 0 || buffer_id == 0 || receive_buffer == NULL ||
        native_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    *native_buffer = NULL;
    advc_put_u64(request + ADVC_TRANSFER_AHB_BUFFER_ID_OFFSET, buffer_id);
    if (transaction_ex(fd, ADVC_OP_TRANSFER_AHB, session_id, request, sizeof(request),
                       NULL, 0, payload, sizeof(payload), &reply, 1) < 0)
        return -1;
    status = parse_status_reply(&reply, payload, detail);
    if (status != ADVC_STATUS_OK) return status;
    if ((reply.header.flags & ADVC_FLAG_AHB_FOLLOWS) == 0 ||
        receive_buffer(fd, native_buffer, userdata) < 0 || *native_buffer == NULL) {
        errno = EPROTO;
        return -1;
    }
    return ADVC_STATUS_OK;
}

int advc_client_flush(int fd, uint32_t session_id, uint32_t *detail) {
    return session_status_transaction(fd, ADVC_OP_FLUSH, session_id,
                                      NULL, 0, NULL, 0, detail);
}

int advc_client_close_session(int fd, uint32_t session_id, uint32_t *detail) {
    return session_status_transaction(fd, ADVC_OP_CLOSE_SESSION, session_id,
                                      NULL, 0, NULL, 0, detail);
}

void advc_client_output_close(struct advc_client_output *output) {
    if (output == NULL) return;
    if (output->data_fd >= 0) close(output->data_fd);
    if (output->acquire_fence_fd >= 0) close(output->acquire_fence_fd);
    output->data_fd = -1;
    output->acquire_fence_fd = -1;
}
