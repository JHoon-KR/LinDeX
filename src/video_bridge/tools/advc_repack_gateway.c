#define _GNU_SOURCE
#include "advc/client.h"
#include "advc/dmabuf_ingress.h"
#include "advc/protocol.h"
#include "turnip_prime_import.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ADVC_GATEWAY_MAX_CLIENTS 16u
#define ADVC_GATEWAY_CACHE_SLOTS \
    (ADVC_MAX_SESSIONS * ADVC_MAX_OUTSTANDING_OUTPUTS)
#define ADVC_DRM_FORMAT_NV12 UINT32_C(0x3231564e)
#define ADVC_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#ifndef ADVC_GATEWAY_IO_TIMEOUT_MS
#define ADVC_GATEWAY_IO_TIMEOUT_MS 5000u
#endif
#ifndef ADVC_GATEWAY_CONNECT_TIMEOUT_MS
#define ADVC_GATEWAY_CONNECT_TIMEOUT_MS ADVC_CLIENT_CONNECT_TIMEOUT_MS
#endif
#ifndef ADVC_GATEWAY_SOCKET_PREFIX
#define ADVC_GATEWAY_SOCKET_PREFIX "/run/android-drm/"
#endif
#define ADVC_GATEWAY_UPSTREAM_MINOR 8u

struct gateway_output {
    int used;
    int upstream_released;
    int reservation;
    uint32_t session_id;
    uint64_t buffer_id;
    uint64_t pts_ns;
    uint64_t repack_lease_token;
    struct advc_dmabuf_descriptor descriptor;
};

struct gateway_client {
    int downstream_fd;
    int upstream_fd;
    const char *upstream_path;
    uint8_t *request_payload;
    uint8_t *reply_payload;
    struct advc_turnip_linear_repack_pool *repack_pool;
    struct gateway_output outputs[ADVC_GATEWAY_CACHE_SLOTS];
    uint64_t timing_dequeue_ns;
    uint64_t timing_transfer_ns;
    uint64_t timing_repack_ns;
    uint64_t timing_release_ns;
    uint64_t timing_send_ns;
    uint64_t timing_count;
    uint64_t next_reservation_id;
};

struct worker_arguments {
    int downstream_fd;
    char upstream_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static atomic_uint active_clients;
static volatile sig_atomic_t stop_requested;
static int listen_fd = -1;
static int trace_enabled;
static int timing_enabled;
enum gateway_output_mode {
    GATEWAY_OUTPUT_AUTO = 0,
    GATEWAY_OUTPUT_LINEAR = 1,
    GATEWAY_OUTPUT_QCOM = 2,
};
static enum gateway_output_mode output_mode;
static int qcom_passthrough_validated;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void record_timing(struct gateway_client *client, uint64_t dequeue_ns,
                          uint64_t transfer_ns, uint64_t repack_ns,
                          uint64_t release_ns, uint64_t send_ns) {
    if (!timing_enabled) return;
    client->timing_dequeue_ns += dequeue_ns;
    client->timing_transfer_ns += transfer_ns;
    client->timing_repack_ns += repack_ns;
    client->timing_release_ns += release_ns;
    client->timing_send_ns += send_ns;
    ++client->timing_count;
    if (client->timing_count % 120u == 0) {
        const uint64_t count = client->timing_count;
        fprintf(stderr,
                "advc-repack-timing: count=%llu dequeue_us=%llu "
                "transfer_us=%llu repack_us=%llu release_us=%llu "
                "send_us=%llu\n",
                (unsigned long long)count,
                (unsigned long long)(client->timing_dequeue_ns / count /
                                     UINT64_C(1000)),
                (unsigned long long)(client->timing_transfer_ns / count /
                                     UINT64_C(1000)),
                (unsigned long long)(client->timing_repack_ns / count /
                                     UINT64_C(1000)),
                (unsigned long long)(client->timing_release_ns / count /
                                     UINT64_C(1000)),
                (unsigned long long)(client->timing_send_ns / count /
                                     UINT64_C(1000)));
    }
}

static int exact_env(const char *name, const char *expected) {
    const char *value = getenv(name);
    return value != NULL && strcmp(value, expected) == 0;
}

static int configure_output_policy(void) {
    const char *value = getenv("ADVC_REPACK_GATEWAY_OUTPUT");
    output_mode = GATEWAY_OUTPUT_AUTO;
    if (value != NULL && value[0] != '\0' && strcmp(value, "auto") != 0) {
        if (strcmp(value, "linear") == 0)
            output_mode = GATEWAY_OUTPUT_LINEAR;
        else if (strcmp(value, "qcom") == 0)
            output_mode = GATEWAY_OUTPUT_QCOM;
        else {
            errno = EINVAL;
            return -1;
        }
    }
    qcom_passthrough_validated = exact_env(
        "ADVC_REPACK_GATEWAY_QCOM_PASSTHROUGH",
        "validated-downstream-import-v1");
    if (output_mode == GATEWAY_OUTPUT_QCOM &&
        !qcom_passthrough_validated) {
        errno = ENOTSUP;
        return -1;
    }
    if (trace_enabled)
        fprintf(stderr,
                "advc-repack-gateway: output-policy mode=%s "
                "qcom-passthrough=%d async-linear=%d\n",
                output_mode == GATEWAY_OUTPUT_LINEAR ? "linear" :
                output_mode == GATEWAY_OUTPUT_QCOM ? "qcom" : "auto",
                qcom_passthrough_validated,
                output_mode != GATEWAY_OUTPUT_QCOM);
    return 0;
}

static int set_socket_watchdog(int fd, int receive_enabled,
                               int send_enabled) {
    struct timeval timeout;
    timeout.tv_sec = ADVC_GATEWAY_IO_TIMEOUT_MS / 1000u;
    timeout.tv_usec =
        (suseconds_t)(ADVC_GATEWAY_IO_TIMEOUT_MS % 1000u) * 1000;
    if (receive_enabled &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0)
        return -1;
    if (send_enabled &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) < 0)
        return -1;
    return 0;
}

static void trace_repack(uint32_t session_id, uint64_t buffer_id,
                         uint64_t source_modifier) {
    if (!trace_enabled) return;
    fprintf(stderr,
            "advc-repack-gateway: repack session=%u buffer=%llu "
            "source_modifier=0x%016llx destination_modifier=0x0\n",
            session_id, (unsigned long long)buffer_id,
            (unsigned long long)source_modifier);
}

static void initialize_descriptor(struct advc_dmabuf_descriptor *descriptor) {
    memset(descriptor, 0, sizeof(*descriptor));
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
}

static void forget_output(struct gateway_output *output) {
    if (output == NULL) return;
    advc_dmabuf_descriptor_close(&output->descriptor);
    memset(output, 0, sizeof(*output));
    initialize_descriptor(&output->descriptor);
}

static void discard_output(struct gateway_client *client,
                           struct gateway_output *output) {
    if (output == NULL) return;
    if (output->repack_lease_token != 0 && client->repack_pool != NULL)
        (void)advc_turnip_linear_repack_pool_discard(
            client->repack_pool, output->repack_lease_token);
    forget_output(output);
}

static void initialize_client(struct gateway_client *client) {
    client->upstream_fd = -1;
    client->next_reservation_id = UINT64_C(1) << 63;
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i)
        initialize_descriptor(&client->outputs[i].descriptor);
}

static void clear_client(struct gateway_client *client) {
    if (client == NULL) return;
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i)
        discard_output(client, &client->outputs[i]);
    advc_turnip_linear_repack_pool_destroy(client->repack_pool);
    client->repack_pool = NULL;
    if (client->upstream_fd >= 0) close(client->upstream_fd);
    if (client->downstream_fd >= 0) close(client->downstream_fd);
    free(client->request_payload);
    free(client->reply_payload);
}

static struct gateway_output *find_output(struct gateway_client *client,
                                          uint32_t session_id,
                                          uint64_t buffer_id) {
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i) {
        struct gateway_output *output = &client->outputs[i];
        if (output->used && output->session_id == session_id &&
            output->buffer_id == buffer_id)
            return output;
    }
    return NULL;
}

static struct gateway_output *find_reservation(
    struct gateway_client *client, uint32_t session_id, uint64_t pts_ns) {
    /* MediaCodec transports presentation timestamps as whole microseconds. */
    pts_ns -= pts_ns % UINT64_C(1000);
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i) {
        struct gateway_output *output = &client->outputs[i];
        if (output->used && output->reservation &&
            output->session_id == session_id && output->pts_ns == pts_ns)
            return output;
    }
    return NULL;
}

static uint64_t allocate_reservation_id(struct gateway_client *client,
                                        uint32_t session_id) {
    uint64_t candidate;
    do {
        candidate = client->next_reservation_id++;
        if (client->next_reservation_id == 0)
            client->next_reservation_id = UINT64_C(1) << 63;
    } while (candidate == 0 ||
             find_output(client, session_id, candidate) != NULL);
    return candidate;
}

static struct gateway_output *allocate_output(struct gateway_client *client,
                                              uint32_t session_id,
                                              uint64_t buffer_id) {
    if (find_output(client, session_id, buffer_id) != NULL) {
        errno = EEXIST;
        return NULL;
    }
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i) {
        struct gateway_output *output = &client->outputs[i];
        if (!output->used) {
            discard_output(client, output);
            output->used = 1;
            output->session_id = session_id;
            output->buffer_id = buffer_id;
            return output;
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void clear_session_outputs(struct gateway_client *client,
                                  uint32_t session_id) {
    for (size_t i = 0; i < ADVC_GATEWAY_CACHE_SLOTS; ++i)
        if (client->outputs[i].used &&
            client->outputs[i].session_id == session_id)
            discard_output(client, &client->outputs[i]);
}

static int ensure_upstream(struct gateway_client *client) {
    if (client->upstream_fd >= 0) return 0;
    client->upstream_fd = advc_client_connect_bounded(
        client->upstream_path, ADVC_GATEWAY_CONNECT_TIMEOUT_MS);
    if (client->upstream_fd < 0) return -1;
    if (set_socket_watchdog(client->upstream_fd, 1, 1) < 0) {
        int saved = errno;
        close(client->upstream_fd);
        client->upstream_fd = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

static void poison_upstream(struct gateway_client *client) {
    if (client->upstream_fd < 0) return;
    (void)shutdown(client->upstream_fd, SHUT_RDWR);
    close(client->upstream_fd);
    client->upstream_fd = -1;
}

static void prepare_message(struct advc_message *message, uint8_t *payload) {
    memset(message, 0, sizeof(*message));
    message->payload = payload;
    message->payload_capacity = ADVC_MAX_PAYLOAD;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) message->fds[i] = -1;
}

static int exchange_upstream(struct gateway_client *client,
                             const struct advc_message *request,
                             struct advc_message *reply) {
    struct advc_message upstream_request;
    if (ensure_upstream(client) < 0) return -1;
    upstream_request = *request;
    if (upstream_request.header.version_minor > ADVC_GATEWAY_UPSTREAM_MINOR)
        upstream_request.header.version_minor = ADVC_GATEWAY_UPSTREAM_MINOR;
    if (advc_send_message(client->upstream_fd, &upstream_request) < 0) {
        poison_upstream(client);
        return -1;
    }
    prepare_message(reply, client->reply_payload);
    if (advc_receive_message(client->upstream_fd, reply) < 0) {
        poison_upstream(client);
        return -1;
    }
    if (reply->header.message_type != ADVC_MSG_REPLY ||
        reply->header.opcode != request->header.opcode ||
        reply->header.request_id != request->header.request_id ||
        (request->header.opcode != ADVC_OP_CREATE_SESSION &&
         reply->header.session_id != request->header.session_id)) {
        advc_close_message_fds(reply);
        poison_upstream(client);
        errno = EPROTO;
        return -1;
    }
    /* Present the gateway's downstream protocol version to its local client. */
    reply->header.version_minor = request->header.version_minor;
    return 0;
}

static int send_status_reply(int downstream_fd,
                             const struct advc_message *request,
                             uint32_t status, uint32_t detail) {
    uint8_t payload[ADVC_STATUS_SIZE] = {0};
    struct advc_message reply;
    memset(&reply, 0, sizeof(reply));
    advc_put_u32(payload + ADVC_STATUS_CODE_OFFSET, status);
    advc_put_u32(payload + ADVC_STATUS_DETAIL_OFFSET, detail);
    reply.header.version_major = request->header.version_major;
    reply.header.version_minor = request->header.version_minor;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.opcode = request->header.opcode;
    reply.header.request_id = request->header.request_id;
    reply.header.session_id = request->header.session_id;
    reply.header.payload_size = sizeof(payload);
    reply.payload = payload;
    return advc_send_message(downstream_fd, &reply);
}

static int reply_status_is_ok(const struct advc_message *reply) {
    return reply->header.payload_size >= ADVC_STATUS_SIZE &&
           advc_get_u32(reply->payload + ADVC_STATUS_CODE_OFFSET) ==
               ADVC_STATUS_OK;
}

static int forward_request(struct gateway_client *client,
                           struct advc_message *request) {
    struct advc_message reply;
    int result;
    result = exchange_upstream(client, request, &reply);
    advc_close_message_fds(request);
    if (result < 0) return -1;
    result = advc_send_message(client->downstream_fd, &reply);
    advc_close_message_fds(&reply);
    return result;
}

static int handle_hello(struct gateway_client *client,
                        struct advc_message *request) {
    struct advc_message reply;
    uint64_t requested_features = UINT64_MAX;
    int result;
    if (request->header.payload_size == ADVC_HELLO_SIZE)
        requested_features = advc_get_u64(
            request->payload + ADVC_HELLO_FEATURES_OFFSET);
    result = exchange_upstream(client, request, &reply);
    advc_close_message_fds(request);
    if (result < 0) return -1;
    if (reply.header.payload_size == ADVC_HELLO_SIZE &&
        reply.header.fd_count == 0) {
        uint64_t features = advc_get_u64(
            reply.payload + ADVC_HELLO_FEATURES_OFFSET);
        if (!qcom_passthrough_validated ||
            output_mode == GATEWAY_OUTPUT_LINEAR)
            features &= ~ADVC_FEATURE_DECODE_QCOM_MODIFIER;
        if (request->header.version_minor >= 8 &&
            output_mode != GATEWAY_OUTPUT_QCOM &&
            (requested_features & ADVC_FEATURE_ASYNC_DECODE_PRIME) != 0)
            features |= ADVC_FEATURE_ASYNC_DECODE_PRIME;
        else
            features &= ~ADVC_FEATURE_ASYNC_DECODE_PRIME;
        advc_put_u64(reply.payload + ADVC_HELLO_FEATURES_OFFSET,
                     features);
    }
    result = advc_send_message(client->downstream_fd, &reply);
    advc_close_message_fds(&reply);
    return result;
}

static int send_cached_prime(struct gateway_client *client,
                             const struct advc_message *request,
                             const struct gateway_output *output) {
    uint8_t payload[ADVC_TRANSFER_PRIME_REPLY_SIZE] = {0};
    struct advc_message reply;
    int object_fds[ADVC_MAX_DMABUF_OBJECTS];
    uint16_t object_count = 0;
    if (advc_dmabuf_registration_encode(
            payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
            &output->descriptor, object_fds, &object_count) < 0)
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_INTERNAL, (uint32_t)errno);
    memset(&reply, 0, sizeof(reply));
    advc_put_u32(payload + ADVC_STATUS_CODE_OFFSET, ADVC_STATUS_OK);
    reply.header.version_major = request->header.version_major;
    reply.header.version_minor = request->header.version_minor;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.opcode = request->header.opcode;
    reply.header.request_id = request->header.request_id;
    reply.header.session_id = request->header.session_id;
    reply.header.payload_size = sizeof(payload);
    reply.header.fd_count = object_count;
    reply.payload = payload;
    for (uint16_t i = 0; i < object_count; ++i) reply.fds[i] = object_fds[i];
    return advc_send_message(client->downstream_fd, &reply);
}

static int handle_reserve_linear(struct gateway_client *client,
                                 struct advc_message *request) {
    struct advc_turnip_linear_repack_result reserved;
    struct gateway_output *output = NULL;
    uint64_t pts_ns;
    uint64_t reservation_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t detail;
    int result;

    if (request->header.version_minor < 8 ||
        request->header.payload_size != ADVC_RESERVE_LINEAR_SIZE ||
        request->header.fd_count != 0) {
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_BAD_MESSAGE, EINVAL);
    }
    pts_ns = advc_get_u64(
        request->payload + ADVC_RESERVE_LINEAR_PTS_NS_OFFSET);
    width = advc_get_u32(
        request->payload + ADVC_RESERVE_LINEAR_WIDTH_OFFSET);
    height = advc_get_u32(
        request->payload + ADVC_RESERVE_LINEAR_HEIGHT_OFFSET);
    fourcc = advc_get_u32(
        request->payload + ADVC_RESERVE_LINEAR_FOURCC_OFFSET);
    for (size_t i = ADVC_RESERVE_LINEAR_FOURCC_OFFSET + sizeof(uint32_t);
         i < ADVC_RESERVE_LINEAR_SIZE; ++i) {
        if (request->payload[i] != 0) {
            advc_close_message_fds(request);
            return send_status_reply(client->downstream_fd, request,
                                     ADVC_STATUS_BAD_MESSAGE, EINVAL);
        }
    }
    if (pts_ns > (uint64_t)INT64_MAX || width < 16 || width > 8192 ||
        height < 16 || height > 8192 || (width & 1u) != 0 ||
        (height & 1u) != 0 || fourcc != ADVC_DRM_FORMAT_NV12 ||
        find_reservation(client, request->header.session_id, pts_ns) != NULL) {
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_BAD_MESSAGE, EINVAL);
    }

    memset(&reserved, 0, sizeof(reserved));
    initialize_descriptor(&reserved.descriptor);
    reserved.acquire_fence_fd = -1;
    reserved.source_release_fence_fd = -1;
    reservation_id = allocate_reservation_id(
        client, request->header.session_id);
    if (advc_turnip_linear_repack_pool_reserve(
            client->repack_pool, width, height, reservation_id, &reserved) < 0) {
        detail = (uint32_t)(errno == 0 ? EIO : errno);
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 detail == ENOSPC ? ADVC_STATUS_WOULD_BLOCK :
                                                   ADVC_STATUS_INTERNAL,
                                 detail);
    }
    output = allocate_output(client, request->header.session_id,
                             reservation_id);
    if (output == NULL) {
        detail = (uint32_t)(errno == 0 ? EIO : errno);
        (void)advc_turnip_linear_repack_pool_discard(
            client->repack_pool, reserved.lease_token);
        reserved.lease_token = 0;
        advc_turnip_linear_repack_close(&reserved);
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 detail == ENOSPC ? ADVC_STATUS_WOULD_BLOCK :
                                                   ADVC_STATUS_INTERNAL,
                                 detail);
    }
    output->upstream_released = 1;
    output->reservation = 1;
    output->pts_ns = pts_ns - pts_ns % UINT64_C(1000);
    output->repack_lease_token = reserved.lease_token;
    reserved.lease_token = 0;
    output->descriptor = reserved.descriptor;
    initialize_descriptor(&reserved.descriptor);
    if (trace_enabled)
        fprintf(stderr,
                "advc-repack-gateway: reserve session=%u pts=%llu token=%llu "
                "lease=%llu size=%ux%u\n",
                request->header.session_id, (unsigned long long)pts_ns,
                (unsigned long long)reservation_id,
                (unsigned long long)output->repack_lease_token, width, height);
    result = send_cached_prime(client, request, output);
    advc_close_message_fds(request);
    advc_turnip_linear_repack_close(&reserved);
    if (result < 0) discard_output(client, output);
    return result;
}

static int release_upstream_output(struct gateway_client *client,
                                   uint32_t session_id, uint64_t buffer_id,
                                   int release_fence_fd) {
    uint32_t detail = 0;
    int status = advc_client_release_output_fenced(
        client->upstream_fd, session_id, buffer_id, release_fence_fd, &detail);
    if (status != ADVC_STATUS_OK) {
        errno = status < 0 ? errno : EIO;
        poison_upstream(client);
        return -1;
    }
    return 0;
}

static int replace_dequeue_with_linear(
    struct gateway_client *client, const struct advc_message *request,
    struct advc_message *reply, struct advc_dmabuf_descriptor *source,
    uint64_t buffer_id, struct gateway_output *reservation,
    uint64_t *repack_ns, uint64_t *release_ns) {
    struct advc_turnip_linear_repack_result repacked;
    struct gateway_output *cached = NULL;
    int source_fence = reply->header.fd_count == 1 ? reply->fds[0] : -1;
    int result = -1;
    uint64_t started_ns;

    memset(&repacked, 0, sizeof(repacked));
    initialize_descriptor(&repacked.descriptor);
    repacked.acquire_fence_fd = -1;
    repacked.source_release_fence_fd = -1;
    started_ns = monotonic_ns();
    if (reservation != NULL) {
        if (!reservation->reservation ||
            reservation->repack_lease_token == 0 ||
            advc_turnip_prime_repack_linear_reserved(
                client->repack_pool, reservation->repack_lease_token, source,
                source_fence, buffer_id, &repacked) < 0)
            goto out;
    } else if (advc_turnip_prime_repack_linear_pooled(
                   client->repack_pool, source, source_fence, buffer_id,
                   &repacked) < 0) {
        goto out;
    }
    *repack_ns = monotonic_ns() - started_ns;
    if (repacked.descriptor.drm_fourcc != ADVC_DRM_FORMAT_NV12 ||
        repacked.descriptor.drm_modifier != 0 ||
        advc_dmabuf_descriptor_validate(&repacked.descriptor) < 0) {
        errno = EPROTO;
        goto out;
    }
    if (reservation != NULL) {
        cached = reservation;
        if (repacked.lease_token != cached->repack_lease_token) {
            errno = EPROTO;
            goto out;
        }
    } else {
        cached = allocate_output(client, request->header.session_id, buffer_id);
        if (cached == NULL) goto out;
    }
    started_ns = monotonic_ns();
    if (release_upstream_output(client, request->header.session_id, buffer_id,
                                repacked.source_release_fence_fd) < 0)
        goto out;
    *release_ns = monotonic_ns() - started_ns;
    close(repacked.source_release_fence_fd);
    repacked.source_release_fence_fd = -1;
    cached->upstream_released = 1;
    if (reservation != NULL) {
        cached->buffer_id = buffer_id;
        cached->descriptor.buffer_id = buffer_id;
        cached->reservation = 0;
        cached->pts_ns = 0;
        repacked.lease_token = 0;
        advc_dmabuf_descriptor_close(&repacked.descriptor);
        initialize_descriptor(&repacked.descriptor);
    } else {
        cached->repack_lease_token = repacked.lease_token;
        repacked.lease_token = 0;
        cached->descriptor = repacked.descriptor;
        initialize_descriptor(&repacked.descriptor);
    }
    trace_repack(request->header.session_id, buffer_id,
                 source->drm_modifier);

    advc_close_message_fds(reply);
    if (repacked.acquire_fence_fd >= 0) {
        reply->fds[0] = repacked.acquire_fence_fd;
        repacked.acquire_fence_fd = -1;
        reply->header.fd_count = 1;
        advc_put_u32(reply->payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET,
                     ADVC_FD_ACQUIRE_FENCE);
    } else {
        reply->header.fd_count = 0;
        advc_put_u32(reply->payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET,
                     ADVC_FD_NONE);
    }
    result = 0;
out:
    if (result < 0 && trace_enabled)
        fprintf(stderr,
                "advc-repack-gateway: repack-fail session=%u buffer=%llu "
                "reserved=%u errno=%d\n",
                request->header.session_id, (unsigned long long)buffer_id,
                reservation != NULL, errno);
    if (result < 0 && reservation != NULL) {
        repacked.lease_token = 0;
        discard_output(client, reservation);
        cached = NULL;
    }
    if (result < 0 && repacked.lease_token != 0) {
        (void)advc_turnip_linear_repack_pool_discard(
            client->repack_pool, repacked.lease_token);
        repacked.lease_token = 0;
    }
    advc_turnip_linear_repack_close(&repacked);
    if (result < 0 && cached != NULL) discard_output(client, cached);
    return result;
}

static int cache_direct(struct gateway_client *client,
                        const struct advc_message *request,
                        struct advc_dmabuf_descriptor *source,
                        uint64_t buffer_id) {
    struct gateway_output *cached =
        allocate_output(client, request->header.session_id, buffer_id);
    if (cached == NULL) return -1;
    cached->upstream_released = 0;
    cached->descriptor = *source;
    initialize_descriptor(source);
    return 0;
}

static int handle_dequeue(struct gateway_client *client,
                          struct advc_message *request) {
    struct advc_message reply;
    struct advc_dmabuf_descriptor source;
    struct gateway_output *reservation = NULL;
    uint64_t buffer_id = 0;
    uint64_t pts_ns = 0;
    uint32_t detail = 0;
    uint64_t dequeue_ns = 0;
    uint64_t transfer_ns = 0;
    uint64_t repack_ns = 0;
    uint64_t release_ns = 0;
    uint64_t send_ns = 0;
    uint64_t started_ns;
    int failure_errno;
    int status;
    int result;

    initialize_descriptor(&source);
    started_ns = monotonic_ns();
    if (exchange_upstream(client, request, &reply) < 0) {
        advc_close_message_fds(request);
        return -1;
    }
    dequeue_ns = monotonic_ns() - started_ns;
    advc_close_message_fds(request);
    if (reply.header.payload_size == ADVC_STATUS_SIZE ||
        (reply.header.payload_size == ADVC_OUTPUT_BYTES_SIZE &&
         advc_get_u32(reply.payload + ADVC_OUTPUT_TRANSPORT_OFFSET) ==
             ADVC_TRANSPORT_BYTES)) {
        result = advc_send_message(client->downstream_fd, &reply);
        advc_close_message_fds(&reply);
        return result;
    }
    if (reply.header.payload_size != ADVC_OUTPUT_READY_SIZE ||
        advc_get_u32(reply.payload + ADVC_OUTPUT_TRANSPORT_OFFSET) !=
            ADVC_TRANSPORT_AHARDWAREBUFFER ||
        reply.header.fd_count > 1 ||
        (advc_get_u32(reply.payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET) ==
             ADVC_FD_ACQUIRE_FENCE) != (reply.header.fd_count == 1) ||
        (reply.header.fd_count == 0 &&
         advc_get_u32(reply.payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET) !=
             ADVC_FD_NONE)) {
        errno = EPROTO;
        goto fail_output;
    }
    buffer_id = advc_get_u64(reply.payload + ADVC_OUTPUT_BUFFER_ID_OFFSET);
    if (buffer_id == 0) {
        errno = EPROTO;
        goto fail_output;
    }
    pts_ns = advc_get_u64(reply.payload + ADVC_OUTPUT_PTS_NS_OFFSET);
    reservation = find_reservation(client, request->header.session_id, pts_ns);
    started_ns = monotonic_ns();
    status = advc_client_transfer_prime(client->upstream_fd,
                                        request->header.session_id, buffer_id,
                                        &source, &detail);
    transfer_ns = monotonic_ns() - started_ns;
    if (status < 0) {
        poison_upstream(client);
        goto fail_output;
    }
    if (status != ADVC_STATUS_OK) goto fail_output;
    if (trace_enabled)
        fprintf(stderr,
                "advc-repack-gateway: source session=%u buffer=%llu "
                "pts=%llu reserved=%u size=%ux%u crop=%u,%u+%ux%u "
                "planes=%u objects=%u pitch=%u,%u offset=%llu,%llu\n",
                request->header.session_id, (unsigned long long)buffer_id,
                (unsigned long long)pts_ns, reservation != NULL,
                source.width, source.height, source.crop_left,
                source.crop_top, source.crop_width, source.crop_height,
                source.plane_count, source.object_count,
                source.planes[0].pitch, source.planes[1].pitch,
                (unsigned long long)source.planes[0].offset,
                (unsigned long long)source.planes[1].offset);

    if (source.drm_fourcc == ADVC_DRM_FORMAT_NV12 &&
        source.drm_modifier == ADVC_QCOM_COMPRESSED) {
        if (reservation != NULL || output_mode == GATEWAY_OUTPUT_LINEAR ||
            !qcom_passthrough_validated) {
            if (output_mode == GATEWAY_OUTPUT_QCOM) {
                errno = ENOTSUP;
                goto fail_output;
            }
            if (replace_dequeue_with_linear(client, request, &reply, &source,
                                            buffer_id, reservation,
                                            &repack_ns, &release_ns) < 0)
                goto fail_output;
        } else if (cache_direct(client, request, &source, buffer_id) < 0) {
            goto fail_output;
        } else if (trace_enabled) {
            fprintf(stderr,
                    "advc-repack-gateway: passthrough-qcom session=%u "
                    "buffer=%llu modifier=0x%016llx\n",
                    request->header.session_id,
                    (unsigned long long)buffer_id,
                    (unsigned long long)source.drm_modifier);
        }
    } else if (source.drm_fourcc == ADVC_DRM_FORMAT_NV12 &&
               source.drm_modifier == 0) {
        if (reservation != NULL || output_mode == GATEWAY_OUTPUT_QCOM) {
            errno = ENOTSUP;
            goto fail_output;
        }
        if (cache_direct(client, request, &source, buffer_id) < 0)
            goto fail_output;
    } else {
        errno = ENOTSUP;
        goto fail_output;
    }

    advc_dmabuf_descriptor_close(&source);
    started_ns = monotonic_ns();
    result = advc_send_message(client->downstream_fd, &reply);
    send_ns = monotonic_ns() - started_ns;
    if (result >= 0)
        record_timing(client, dequeue_ns, transfer_ns, repack_ns, release_ns,
                      send_ns);
    advc_close_message_fds(&reply);
    return result;

fail_output:
    failure_errno = errno;
    if (trace_enabled)
        fprintf(stderr,
                "advc-repack-gateway: dequeue-fail session=%u buffer=%llu "
                "pts=%llu reserved=%u fourcc=0x%08x modifier=0x%016llx "
                "errno=%d\n",
                request->header.session_id, (unsigned long long)buffer_id,
                (unsigned long long)pts_ns, reservation != NULL,
                source.drm_fourcc, (unsigned long long)source.drm_modifier,
                failure_errno);
    detail = (uint32_t)failure_errno;
    if (reservation != NULL) discard_output(client, reservation);
    advc_dmabuf_descriptor_close(&source);
    advc_close_message_fds(&reply);
    if (failure_errno == EPROTO) {
        poison_upstream(client);
        return -1;
    }
    if (buffer_id != 0 && client->upstream_fd >= 0 &&
        release_upstream_output(client, request->header.session_id,
                                buffer_id, -1) < 0) {
        poison_upstream(client);
        return -1;
    }
    if (buffer_id != 0 && client->upstream_fd < 0) return -1;
    return send_status_reply(client->downstream_fd, request,
                             failure_errno == ENOTSUP ?
                                 ADVC_STATUS_UNSUPPORTED : ADVC_STATUS_INTERNAL,
                             detail);
}

static int handle_transfer_prime(struct gateway_client *client,
                                 struct advc_message *request) {
    struct gateway_output *output;
    uint64_t buffer_id;
    if (request->header.payload_size != ADVC_TRANSFER_PRIME_SIZE ||
        request->header.fd_count != 0) {
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_BAD_MESSAGE, EINVAL);
    }
    buffer_id = advc_get_u64(
        request->payload + ADVC_TRANSFER_PRIME_BUFFER_ID_OFFSET);
    output = find_output(client, request->header.session_id, buffer_id);
    if (output == NULL) return forward_request(client, request);
    return send_cached_prime(client, request, output);
}

static int handle_release_output(struct gateway_client *client,
                                 struct advc_message *request) {
    struct gateway_output *output;
    struct advc_message reply;
    uint64_t buffer_id;
    uint32_t fence_role;
    int release_fence_fd;
    int result;
    if (request->header.payload_size != ADVC_RELEASE_OUTPUT_SIZE ||
        request->header.fd_count > 1) {
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_BAD_MESSAGE, EINVAL);
    }
    fence_role = advc_get_u32(
        request->payload + ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET);
    for (size_t i = ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET + sizeof(uint32_t);
         i < ADVC_RELEASE_OUTPUT_SIZE; ++i) {
        if (request->payload[i] != 0) {
            advc_close_message_fds(request);
            return send_status_reply(client->downstream_fd, request,
                                     ADVC_STATUS_BAD_MESSAGE, EINVAL);
        }
    }
    if ((fence_role == ADVC_FD_NONE && request->header.fd_count != 0) ||
        (fence_role == ADVC_FD_RELEASE_FENCE &&
         request->header.fd_count != 1) ||
        (fence_role != ADVC_FD_NONE &&
         fence_role != ADVC_FD_RELEASE_FENCE)) {
        advc_close_message_fds(request);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_BAD_MESSAGE, EINVAL);
    }
    buffer_id = advc_get_u64(
        request->payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET);
    output = find_output(client, request->header.session_id, buffer_id);
    if (output == NULL) return forward_request(client, request);
    if (output->upstream_released) {
        release_fence_fd = request->header.fd_count == 1 ? request->fds[0] : -1;
        if (output->repack_lease_token == 0 ||
            advc_turnip_linear_repack_pool_release(
                client->repack_pool, output->repack_lease_token,
                release_fence_fd) < 0) {
            uint32_t detail = (uint32_t)(errno == 0 ? EIO : errno);
            advc_close_message_fds(request);
            discard_output(client, output);
            return send_status_reply(client->downstream_fd, request,
                                     ADVC_STATUS_INTERNAL, detail);
        }
        advc_close_message_fds(request);
        output->repack_lease_token = 0;
        forget_output(output);
        return send_status_reply(client->downstream_fd, request,
                                 ADVC_STATUS_OK, 0);
    }
    result = exchange_upstream(client, request, &reply);
    advc_close_message_fds(request);
    if (result < 0) return -1;
    result = advc_send_message(client->downstream_fd, &reply);
    if (reply_status_is_ok(&reply)) discard_output(client, output);
    advc_close_message_fds(&reply);
    return result;
}

static int handle_flush_or_close(struct gateway_client *client,
                                 struct advc_message *request) {
    struct advc_message reply;
    int result;
    result = exchange_upstream(client, request, &reply);
    advc_close_message_fds(request);
    if (result < 0) return -1;
    result = advc_send_message(client->downstream_fd, &reply);
    if (reply_status_is_ok(&reply))
        clear_session_outputs(client, request->header.session_id);
    advc_close_message_fds(&reply);
    return result;
}

static void *client_worker(void *opaque) {
    struct worker_arguments *arguments = opaque;
    struct gateway_client client;
    struct advc_message request;

    memset(&client, 0, sizeof(client));
    client.downstream_fd = arguments->downstream_fd;
    client.upstream_path = arguments->upstream_path;
    initialize_client(&client);
    client.repack_pool = advc_turnip_linear_repack_pool_create(
        ADVC_GATEWAY_CACHE_SLOTS);
    client.request_payload = malloc(ADVC_MAX_PAYLOAD);
    client.reply_payload = malloc(ADVC_MAX_PAYLOAD);
    if (client.repack_pool == NULL || client.request_payload == NULL ||
        client.reply_payload == NULL)
        goto out;

    while (!stop_requested) {
        int result;
        prepare_message(&request, client.request_payload);
        if (advc_receive_message(client.downstream_fd, &request) < 0) break;
        if (request.header.message_type != ADVC_MSG_REQUEST) {
            advc_close_message_fds(&request);
            break;
        }
        switch (request.header.opcode) {
        case ADVC_OP_HELLO:
            result = handle_hello(&client, &request);
            break;
        case ADVC_OP_RESERVE_LINEAR:
            result = handle_reserve_linear(&client, &request);
            break;
        case ADVC_OP_DEQUEUE_OUTPUT:
            result = handle_dequeue(&client, &request);
            break;
        case ADVC_OP_TRANSFER_PRIME:
            result = handle_transfer_prime(&client, &request);
            break;
        case ADVC_OP_RELEASE_OUTPUT:
            result = handle_release_output(&client, &request);
            break;
        case ADVC_OP_FLUSH:
        case ADVC_OP_CLOSE_SESSION:
            result = handle_flush_or_close(&client, &request);
            break;
        case ADVC_OP_TRANSFER_AHB:
        case ADVC_OP_QUEUE_AHB:
            advc_close_message_fds(&request);
            result = send_status_reply(client.downstream_fd, &request,
                                       ADVC_STATUS_UNSUPPORTED, ENOTSUP);
            break;
        default:
            result = forward_request(&client, &request);
            break;
        }
        if (result < 0) break;
    }
out:
    clear_client(&client);
    atomic_fetch_sub_explicit(&active_clients, 1, memory_order_relaxed);
    free(arguments);
    return NULL;
}

static int peer_is_root(int fd) {
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.uid == 0;
}

static int create_listener(const char *path) {
    struct sockaddr_un address;
    const size_t prefix_length = sizeof(ADVC_GATEWAY_SOCKET_PREFIX) - 1u;
    int bound = 0;
    int fd;
    if (path == NULL ||
        strncmp(path, ADVC_GATEWAY_SOCKET_PREFIX, prefix_length) != 0 ||
        path[prefix_length] == '\0' ||
        strlen(path) >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        bound = 1;
    } else if (errno == EADDRINUSE) {
        struct stat node;
        int probe;
        int stale = 0;
        if (lstat(path, &node) == 0 && S_ISSOCK(node.st_mode) &&
            node.st_uid == geteuid()) {
            probe = advc_client_connect_bounded(
                path, ADVC_GATEWAY_CONNECT_TIMEOUT_MS);
            if (probe < 0 &&
                (errno == ECONNREFUSED || errno == ENOENT))
                stale = 1;
            if (probe >= 0) close(probe);
        }
        if (stale && unlink(path) == 0 &&
            bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
            bound = 1;
    }
    if (!bound || chmod(path, 0600) < 0 ||
        listen(fd, ADVC_GATEWAY_MAX_CLIENTS) < 0) {
        int saved = errno;
        close(fd);
        if (bound) unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

static void handle_signal(int signal_number) {
    int fd;
    (void)signal_number;
    stop_requested = 1;
    fd = listen_fd;
    listen_fd = -1;
    if (fd >= 0) close(fd);
}

#ifndef ADVC_GATEWAY_NO_MAIN
int main(int argc, char **argv) {
    const char *listen_path;
    const char *upstream_path;
    struct sigaction action;
    pid_t parent_pid;
    int result = 1;

    if (argc != 3 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "usage: %s LISTEN_SOCKET UPSTREAM_SOCKET\n", argv[0]);
        return argc == 2 ? 0 : 64;
    }
    if (!exact_env("ADVC_REPACK_GATEWAY_ENABLE",
                   "validated-qcom-nv12-v1")) {
        fprintf(stderr,
                "advc-repack-gateway: exact validation gate is required\n");
        return 78;
    }
    trace_enabled = exact_env("ADVC_REPACK_GATEWAY_TRACE", "1");
    timing_enabled = exact_env("ADVC_REPACK_GATEWAY_TIMING", "1");
    if (configure_output_policy() < 0) {
        fprintf(stderr,
                "advc-repack-gateway: invalid output policy errno=%d\n",
                errno);
        return 78;
    }
    parent_pid = getppid();
    if (parent_pid <= 1 || prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 ||
        getppid() != parent_pid) {
        fprintf(stderr,
                "advc-repack-gateway: parent lifetime gate failed\n");
        return 1;
    }
    listen_path = argv[1];
    upstream_path = argv[2];
    if (strcmp(listen_path, upstream_path) == 0 ||
        strncmp(upstream_path, ADVC_GATEWAY_SOCKET_PREFIX,
                sizeof(ADVC_GATEWAY_SOCKET_PREFIX) - 1u) != 0 ||
        strlen(upstream_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "gateway sockets must be distinct\n");
        return 64;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = create_listener(listen_path);
    if (listen_fd < 0) {
        fprintf(stderr, "advc-repack-gateway: listen failed: %s\n",
                strerror(errno));
        return 1;
    }
    while (!stop_requested) {
        struct worker_arguments *arguments;
        pthread_t thread;
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (stop_requested || errno == EBADF) break;
            fprintf(stderr, "advc-repack-gateway: accept failed: %s\n",
                    strerror(errno));
            goto out;
        }
        if (!peer_is_root(client_fd) ||
            atomic_load_explicit(&active_clients, memory_order_relaxed) >=
                ADVC_GATEWAY_MAX_CLIENTS) {
            close(client_fd);
            continue;
        }
        if (set_socket_watchdog(client_fd, 0, 1) < 0) {
            close(client_fd);
            continue;
        }
        arguments = calloc(1, sizeof(*arguments));
        if (arguments == NULL) {
            close(client_fd);
            continue;
        }
        arguments->downstream_fd = client_fd;
        memcpy(arguments->upstream_path, upstream_path,
               strlen(upstream_path) + 1);
        atomic_fetch_add_explicit(&active_clients, 1, memory_order_relaxed);
        if (pthread_create(&thread, NULL, client_worker, arguments) != 0) {
            atomic_fetch_sub_explicit(&active_clients, 1,
                                      memory_order_relaxed);
            close(client_fd);
            free(arguments);
            continue;
        }
        pthread_detach(thread);
    }
    result = 0;
out:
    if (listen_fd >= 0) close(listen_fd);
    unlink(listen_path);
    return result;
}
#endif
