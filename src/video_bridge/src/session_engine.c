#define _GNU_SOURCE
#include "advc/session_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002u
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

struct advc_output_slot {
    uint64_t id;
    uintptr_t backend_token;
    void *native_buffer;
    struct advc_ahb_public_metadata prime_metadata;
    uint32_t transport;
    int transferred;
    int prime_transferred;
    int in_use;
};

struct advc_dmabuf_completion_slot {
    uint64_t buffer_id;
    uint32_t status;
    uint32_t detail;
    int release_fence_fd;
    int in_use;
};

struct advc_session_slot {
    uint32_t id;
    void *backend_handle;
    struct advc_output_slot outputs[ADVC_MAX_OUTSTANDING_OUTPUTS];
    size_t encode_frame_size;
    uint32_t transport;
    uint8_t direction;
    struct advc_backend_ahb_input pending_ahb;
    uint32_t pending_ahb_status;
    uint32_t pending_ahb_detail;
    int pending_ahb_release_fence;
    uint8_t pending_ahb_state; /* 0 none, 1 await handle, 2 complete */
    struct advc_dmabuf_registry *dmabufs;
    struct advc_dmabuf_completion_slot
        dmabuf_completions[ADVC_MAX_INFLIGHT_DMABUFS];
    struct advc_session_engine *engine;
    int in_use;
};

struct advc_session_engine {
    struct advc_backend_ops ops;
    void *backend_userdata;
    struct advc_session_slot sessions[ADVC_MAX_SESSIONS];
    uint32_t next_session_id;
    uint64_t next_output_id;
};

static void write_status(struct advc_message *reply, uint32_t status, uint32_t detail) {
    advc_put_u32(reply->payload + ADVC_STATUS_CODE_OFFSET, status);
    advc_put_u32(reply->payload + ADVC_STATUS_DETAIL_OFFSET, detail);
    reply->header.payload_size = ADVC_STATUS_SIZE;
}

static uint32_t dmabuf_errno_status(int error_number) {
    switch (error_number) {
    case EINVAL:
    case EBADF:
    case EEXIST:
    case ENOENT:
    case EBUSY:
    case EALREADY:
        return ADVC_STATUS_BAD_MESSAGE;
    case ENOSPC:
    case ENOMEM:
        return ADVC_STATUS_NO_RESOURCE;
    case EAGAIN:
        return ADVC_STATUS_WOULD_BLOCK;
    case ENOTSUP:
        return ADVC_STATUS_UNSUPPORTED;
    default:
        return ADVC_STATUS_INTERNAL;
    }
}

static int session_dmabuf_format_allowed(
    void *userdata, const struct advc_dmabuf_descriptor *descriptor) {
    struct advc_session_slot *session = (struct advc_session_slot *)userdata;
    if (session == NULL || session->engine == NULL ||
        session->engine->ops.dmabuf_format_allowed == NULL ||
        session->backend_handle == NULL) {
        errno = ENOTSUP;
        return 0;
    }
    return session->engine->ops.dmabuf_format_allowed(
        session->engine->backend_userdata, session->backend_handle, descriptor);
}

static struct advc_dmabuf_completion_slot *free_dmabuf_completion(
    struct advc_session_slot *session) {
    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i) {
        if (!session->dmabuf_completions[i].in_use)
            return &session->dmabuf_completions[i];
    }
    return NULL;
}

static struct advc_dmabuf_completion_slot *find_dmabuf_completion(
    struct advc_session_slot *session, uint64_t buffer_id) {
    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i) {
        if (session->dmabuf_completions[i].in_use &&
            session->dmabuf_completions[i].buffer_id == buffer_id)
            return &session->dmabuf_completions[i];
    }
    return NULL;
}

static struct advc_session_slot *find_session(struct advc_session_engine *engine,
                                              uint32_t id) {
    for (size_t i = 0; i < ADVC_MAX_SESSIONS; ++i) {
        if (engine->sessions[i].in_use && engine->sessions[i].id == id)
            return &engine->sessions[i];
    }
    return NULL;
}

static uint32_t allocate_session_id(struct advc_session_engine *engine) {
    for (;;) {
        uint32_t candidate = engine->next_session_id++;
        if (candidate == 0) continue;
        if (find_session(engine, candidate) == NULL) return candidate;
    }
}

static uint64_t allocate_output_id(struct advc_session_engine *engine) {
    uint64_t candidate = engine->next_output_id++;
    if (candidate == 0) candidate = engine->next_output_id++;
    return candidate;
}

static int write_all_at(int fd, const uint8_t *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t written = pwrite(fd, data + done, size - done, (off_t)done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        done += (size_t)written;
    }
    return 0;
}

static int create_sealed_output(const uint8_t *data, size_t size) {
    int fd;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "advc-output", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#elif defined(__NR_memfd_create)
    fd = (int)syscall(__NR_memfd_create, "advc-output", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    errno = ENOSYS;
    return -1;
#endif
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0 ||
        (size > 0 && write_all_at(fd, data, size) < 0) ||
        fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int input_fd_is_sealed(int fd) {
    int seals = fcntl(fd, F_GET_SEALS);
    int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    return seals >= 0 && (seals & required) == required;
}

static int read_all_at(int fd, uint8_t *data, size_t size, uint64_t offset) {
    size_t done = 0;
    while (done < size) {
        ssize_t received = pread(fd, data + done, size - done, (off_t)(offset + done));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return -1;
        done += (size_t)received;
    }
    return 0;
}

static uint32_t handle_create(struct advc_session_engine *engine,
                              const struct advc_message *request,
                              struct advc_message *reply) {
    struct advc_backend_config config;
    struct advc_session_slot *slot = NULL;
    const char *mime;
    void *handle = NULL;
    uint32_t direction;
    uint32_t status;
    uint64_t encode_frame_size = 0;

    if (request->header.session_id != 0 || request->header.fd_count != 0 ||
        request->header.payload_size != ADVC_CREATE_SIZE) return ADVC_STATUS_BAD_MESSAGE;
    memset(&config, 0, sizeof(config));
    direction = advc_get_u32(request->payload + ADVC_CREATE_DIRECTION_OFFSET);
    config.width = advc_get_u32(request->payload + ADVC_CREATE_WIDTH_OFFSET);
    config.height = advc_get_u32(request->payload + ADVC_CREATE_HEIGHT_OFFSET);
    config.bitrate = advc_get_u32(request->payload + ADVC_CREATE_BITRATE_OFFSET);
    config.framerate_milli = advc_get_u32(request->payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET);
    config.flags = advc_get_u32(request->payload + ADVC_CREATE_FLAGS_OFFSET);
    config.color_format = advc_get_u32(request->payload + ADVC_CREATE_COLOR_FORMAT_OFFSET);
    config.transport = advc_get_u32(request->payload + ADVC_CREATE_TRANSPORT_OFFSET);
    config.encode_profile =
        advc_get_u32(request->payload + ADVC_CREATE_ENCODE_PROFILE_OFFSET);
    if (config.transport == 0) config.transport = ADVC_TRANSPORT_BYTES;
    mime = (const char *)request->payload + ADVC_CREATE_MIME_OFFSET;

    if ((direction != ADVC_DIRECTION_DECODE && direction != ADVC_DIRECTION_ENCODE) ||
        config.flags != 0 ||
        config.width < 16 || config.width > 8192 || config.height < 16 ||
        config.height > 8192 || config.framerate_milli > 240000 ||
        memchr(mime, '\0', ADVC_MAX_MIME) == NULL || strncmp(mime, "video/", 6) != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    if (direction == ADVC_DIRECTION_DECODE) {
        if (config.color_format != 0 ||
            config.encode_profile != ADVC_ENCODE_PROFILE_NONE ||
            (config.transport != ADVC_TRANSPORT_BYTES &&
             config.transport != ADVC_TRANSPORT_AHARDWAREBUFFER))
            return ADVC_STATUS_BAD_MESSAGE;
        if (config.transport == ADVC_TRANSPORT_AHARDWAREBUFFER &&
            request->header.version_minor < 2)
            return ADVC_STATUS_UNSUPPORTED;
    } else {
        if (request->header.version_minor < 7)
            return ADVC_STATUS_UNSUPPORTED;
        if ((strcmp(mime, "video/avc") != 0 && strcmp(mime, "video/hevc") != 0) ||
            config.bitrate == 0 || config.bitrate > ADVC_MAX_ENCODE_BITRATE ||
            config.framerate_milli < 1000 ||
            (config.width & 1u) != 0 || (config.height & 1u) != 0 ||
            (config.transport != ADVC_TRANSPORT_BYTES &&
             config.transport != ADVC_TRANSPORT_BROKER_EGL_SURFACE &&
             config.transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE &&
             config.transport != ADVC_TRANSPORT_DMABUF) ||
            (config.transport == ADVC_TRANSPORT_BYTES &&
             config.color_format != ADVC_COLOR_FORMAT_YUV420_PLANAR &&
             config.color_format != ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR) ||
            ((config.transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
              config.transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
              config.transport == ADVC_TRANSPORT_DMABUF) &&
             config.color_format != 0) ||
            (strcmp(mime, "video/avc") == 0 &&
             config.encode_profile !=
                 ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE &&
             config.encode_profile != ADVC_ENCODE_PROFILE_H264_MAIN &&
             config.encode_profile != ADVC_ENCODE_PROFILE_H264_HIGH) ||
            (strcmp(mime, "video/hevc") == 0 &&
             config.encode_profile != ADVC_ENCODE_PROFILE_HEVC_MAIN))
            return ADVC_STATUS_BAD_MESSAGE;
        if (config.transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE &&
            request->header.version_minor < 3)
            return ADVC_STATUS_UNSUPPORTED;
        if (config.transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE &&
            (request->header.version_minor < 4 ||
             engine->ops.receive_ahb_input == NULL))
            return ADVC_STATUS_UNSUPPORTED;
        if (config.transport == ADVC_TRANSPORT_DMABUF &&
            (request->header.version_minor < 5 ||
             engine->ops.dmabuf_format_allowed == NULL ||
             engine->ops.submit_dmabuf == NULL))
            return ADVC_STATUS_UNSUPPORTED;
        if (config.transport == ADVC_TRANSPORT_BYTES) {
            encode_frame_size = (uint64_t)config.width * (uint64_t)config.height;
            encode_frame_size += encode_frame_size / 2u;
            if (encode_frame_size > ADVC_MAX_INPUT_BYTES || encode_frame_size > SIZE_MAX)
                return ADVC_STATUS_NO_RESOURCE;
        }
    }
    config.direction = (uint8_t)direction;
    for (size_t i = 0; i < ADVC_MAX_SESSIONS; ++i) {
        if (!engine->sessions[i].in_use) {
            slot = &engine->sessions[i];
            break;
        }
    }
    if (slot == NULL) return ADVC_STATUS_NO_RESOURCE;
    strncpy(config.mime, mime, sizeof(config.mime) - 1);
    status = engine->ops.create(engine->backend_userdata, &config, &handle);
    if (status != ADVC_STATUS_OK) return status;
    if (handle == NULL) return ADVC_STATUS_INTERNAL;

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->backend_handle = handle;
    slot->engine = engine;
    slot->direction = config.direction;
    slot->encode_frame_size = (size_t)encode_frame_size;
    slot->transport = config.transport;
    slot->pending_ahb.acquire_fence_fd = -1;
    slot->pending_ahb_release_fence = -1;
    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i)
        slot->dmabuf_completions[i].release_fence_fd = -1;
    if (config.transport == ADVC_TRANSPORT_DMABUF) {
        slot->dmabufs = advc_dmabuf_registry_create(
            session_dmabuf_format_allowed, slot);
        if (slot->dmabufs == NULL) {
            engine->ops.destroy(engine->backend_userdata, handle);
            memset(slot, 0, sizeof(*slot));
            return errno == ENOMEM ? ADVC_STATUS_NO_RESOURCE :
                                     ADVC_STATUS_INTERNAL;
        }
    }
    slot->id = allocate_session_id(engine);
    reply->header.session_id = slot->id;
    write_status(reply, ADVC_STATUS_OK, 0);
    return ADVC_STATUS_OK;
}

static uint32_t load_input(const struct advc_message *request, uint8_t **owned,
                           const uint8_t **data, size_t *size_out) {
    uint64_t offset;
    uint64_t size64;
    uint32_t role;
    *owned = NULL;
    *data = NULL;
    *size_out = 0;
    if (request->header.payload_size < ADVC_QUEUE_INPUT_SIZE) return ADVC_STATUS_BAD_MESSAGE;
    offset = advc_get_u64(request->payload + ADVC_QUEUE_INPUT_DATA_OFFSET);
    size64 = advc_get_u64(request->payload + ADVC_QUEUE_INPUT_SIZE_OFFSET);
    role = advc_get_u32(request->payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET);
    for (size_t i = ADVC_QUEUE_INPUT_FD_ROLE_OFFSET + sizeof(uint32_t);
         i < ADVC_QUEUE_INPUT_SIZE; ++i) {
        if (request->payload[i] != 0) return ADVC_STATUS_BAD_MESSAGE;
    }
    if (size64 > ADVC_MAX_INPUT_BYTES || size64 > SIZE_MAX) return ADVC_STATUS_NO_RESOURCE;
    *size_out = (size_t)size64;

    if (request->header.fd_count == 0) {
        if (role != ADVC_FD_NONE || offset != ADVC_QUEUE_INPUT_SIZE ||
            size64 > UINT32_MAX - ADVC_QUEUE_INPUT_SIZE ||
            request->header.payload_size != ADVC_QUEUE_INPUT_SIZE + (uint32_t)size64)
            return ADVC_STATUS_BAD_MESSAGE;
        *data = request->payload + ADVC_QUEUE_INPUT_SIZE;
        return ADVC_STATUS_OK;
    }
    if (request->header.fd_count == 1 && role == ADVC_FD_INPUT_DATA &&
        request->header.payload_size == ADVC_QUEUE_INPUT_SIZE) {
        struct stat statbuf;
        uint8_t *buffer;
        if (offset > (uint64_t)INT64_MAX || offset + size64 < offset ||
            fstat(request->fds[0], &statbuf) < 0 || !S_ISREG(statbuf.st_mode) ||
            statbuf.st_size < 0 ||
            !input_fd_is_sealed(request->fds[0]) ||
            offset + size64 > (uint64_t)statbuf.st_size)
            return ADVC_STATUS_BAD_MESSAGE;
        buffer = (uint8_t *)malloc(*size_out > 0 ? *size_out : 1);
        if (buffer == NULL) return ADVC_STATUS_NO_RESOURCE;
        if (*size_out > 0 && read_all_at(request->fds[0], buffer, *size_out, offset) < 0) {
            free(buffer);
            return ADVC_STATUS_BAD_MESSAGE;
        }
        *owned = buffer;
        *data = buffer;
        return ADVC_STATUS_OK;
    }
    return ADVC_STATUS_BAD_MESSAGE;
}

static uint32_t handle_queue(struct advc_session_engine *engine,
                             struct advc_session_slot *session,
                             const struct advc_message *request,
                             struct advc_message *reply) {
    const uint32_t allowed = ADVC_FLAG_END_OF_STREAM | ADVC_FLAG_KEY_FRAME |
                             ADVC_FLAG_CODEC_CONFIG;
    uint32_t flags;
    uint8_t *owned;
    const uint8_t *data;
    size_t size;
    uint64_t pts_ns;
    uint32_t status;
    if ((session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
         session->transport == ADVC_TRANSPORT_DMABUF) &&
        (request->header.fd_count != 0 ||
         request->header.payload_size != ADVC_QUEUE_INPUT_SIZE ||
         advc_get_u64(request->payload + ADVC_QUEUE_INPUT_DATA_OFFSET) !=
             ADVC_QUEUE_INPUT_SIZE ||
         advc_get_u64(request->payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) != 0 ||
         advc_get_u32(request->payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) !=
             ADVC_FLAG_END_OF_STREAM ||
         advc_get_u32(request->payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET) !=
             ADVC_FD_NONE))
        return ADVC_STATUS_UNSUPPORTED;
    if (session->transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE &&
        (request->header.fd_count != 0 ||
         request->header.payload_size != ADVC_QUEUE_INPUT_SIZE ||
         advc_get_u64(request->payload + ADVC_QUEUE_INPUT_DATA_OFFSET) !=
             ADVC_QUEUE_INPUT_SIZE ||
         advc_get_u64(request->payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) != 0 ||
         advc_get_u32(request->payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET) !=
             ADVC_FD_NONE))
        return ADVC_STATUS_BAD_MESSAGE;
    status = load_input(request, &owned, &data, &size);
    if (status != ADVC_STATUS_OK) return status;
    flags = advc_get_u32(request->payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET);
    pts_ns = advc_get_u64(request->payload + ADVC_QUEUE_INPUT_PTS_NS_OFFSET);
    if ((flags & ~allowed) != 0) {
        free(owned);
        return ADVC_STATUS_BAD_MESSAGE;
    }
    if (session->direction == ADVC_DIRECTION_ENCODE &&
        (((flags & ADVC_FLAG_END_OF_STREAM) != 0 &&
          (flags != ADVC_FLAG_END_OF_STREAM || size != 0)) ||
         ((flags & ADVC_FLAG_END_OF_STREAM) == 0 &&
          (flags != 0 || size != session->encode_frame_size)))) {
        free(owned);
        return ADVC_STATUS_BAD_MESSAGE;
    }
    status = engine->ops.queue_input(engine->backend_userdata, session->backend_handle,
                                     data, size, pts_ns, flags);
    free(owned);
    if (status == ADVC_STATUS_OK) write_status(reply, status, 0);
    return status;
}

static uint32_t handle_queue_ahb(struct advc_session_engine *engine,
                                 struct advc_session_slot *session,
                                 const struct advc_message *request,
                                 struct advc_message *reply) {
    struct advc_backend_ahb_input input;
    uint32_t role;
    int fence = -1;
    (void)engine;
    if (request->header.version_minor < 4 ||
        session->transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE)
        return ADVC_STATUS_UNSUPPORTED;
    if (session->pending_ahb_state != 0) return ADVC_STATUS_WOULD_BLOCK;
    if (request->header.payload_size != ADVC_QUEUE_AHB_SIZE ||
        request->header.fd_count > 1)
        return ADVC_STATUS_BAD_MESSAGE;
    for (size_t i = ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET + sizeof(uint32_t);
         i < ADVC_QUEUE_AHB_SIZE; ++i) {
        if (request->payload[i] != 0) return ADVC_STATUS_BAD_MESSAGE;
    }
    memset(&input, 0, sizeof(input));
    input.pts_ns = advc_get_u64(request->payload + ADVC_QUEUE_AHB_PTS_NS_OFFSET);
    input.width = advc_get_u32(request->payload + ADVC_QUEUE_AHB_WIDTH_OFFSET);
    input.height = advc_get_u32(request->payload + ADVC_QUEUE_AHB_HEIGHT_OFFSET);
    input.format = advc_get_u32(request->payload + ADVC_QUEUE_AHB_FORMAT_OFFSET);
    input.layers = advc_get_u32(request->payload + ADVC_QUEUE_AHB_LAYERS_OFFSET);
    input.usage = advc_get_u64(request->payload + ADVC_QUEUE_AHB_USAGE_OFFSET);
    role = advc_get_u32(request->payload + ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET);
    if (input.width == 0 || input.height == 0 || input.layers != 1 ||
        input.pts_ns > (uint64_t)INT64_MAX ||
        advc_get_u32(request->payload + ADVC_QUEUE_AHB_FLAGS_OFFSET) != 0 ||
        ((role == ADVC_FD_NONE) != (request->header.fd_count == 0)) ||
        (request->header.fd_count == 1 && role != ADVC_FD_ACQUIRE_FENCE))
        return ADVC_STATUS_BAD_MESSAGE;
    if (request->header.fd_count == 1) {
        fence = fcntl(request->fds[0], F_DUPFD_CLOEXEC, 0);
        if (fence < 0) return ADVC_STATUS_INTERNAL;
    }
    input.acquire_fence_fd = fence;
    session->pending_ahb = input;
    session->pending_ahb_state = 1;
    session->pending_ahb_status = ADVC_STATUS_INTERNAL;
    session->pending_ahb_detail = 0;
    session->pending_ahb_release_fence = -1;
    write_status(reply, ADVC_STATUS_OK, 0);
    reply->header.flags = ADVC_FLAG_AHB_FOLLOWS;
    return ADVC_STATUS_OK;
}

static uint32_t handle_complete_ahb(struct advc_session_slot *session,
                                    const struct advc_message *request,
                                    struct advc_message *reply) {
    if (request->header.version_minor < 4 ||
        session->transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE)
        return ADVC_STATUS_UNSUPPORTED;
    if (request->header.payload_size != 0 || request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    if (session->pending_ahb_state != 2) return ADVC_STATUS_WOULD_BLOCK;
    memset(reply->payload, 0, ADVC_COMPLETE_AHB_SIZE);
    advc_put_u32(reply->payload + ADVC_COMPLETE_AHB_STATUS_OFFSET,
                 session->pending_ahb_status);
    advc_put_u32(reply->payload + ADVC_COMPLETE_AHB_DETAIL_OFFSET,
                 session->pending_ahb_detail);
    advc_put_u32(reply->payload + ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET,
                 session->pending_ahb_release_fence >= 0 ?
                 ADVC_FD_RELEASE_FENCE : ADVC_FD_NONE);
    reply->header.payload_size = ADVC_COMPLETE_AHB_SIZE;
    if (session->pending_ahb_release_fence >= 0) {
        reply->header.fd_count = 1;
        reply->fds[0] = session->pending_ahb_release_fence;
        session->pending_ahb_release_fence = -1;
    }
    memset(&session->pending_ahb, 0, sizeof(session->pending_ahb));
    session->pending_ahb.acquire_fence_fd = -1;
    session->pending_ahb_state = 0;
    return ADVC_STATUS_OK;
}

static uint32_t handle_register_dmabuf(struct advc_session_slot *session,
                                       const struct advc_message *request,
                                       struct advc_message *reply) {
    struct advc_dmabuf_descriptor descriptor;
    if (request->header.version_minor < 5 ||
        session->transport != ADVC_TRANSPORT_DMABUF ||
        session->dmabufs == NULL)
        return ADVC_STATUS_UNSUPPORTED;
    if (advc_dmabuf_registration_decode(
            request->payload, request->header.payload_size, request->fds,
            request->header.fd_count, &descriptor) < 0)
        return dmabuf_errno_status(errno);
    if (advc_dmabuf_registry_register(session->dmabufs, &descriptor) < 0)
        return dmabuf_errno_status(errno);
    write_status(reply, ADVC_STATUS_OK, 0);
    return ADVC_STATUS_OK;
}

static uint32_t handle_unregister_dmabuf(struct advc_session_slot *session,
                                         const struct advc_message *request,
                                         struct advc_message *reply) {
    uint64_t buffer_id;
    if (request->header.version_minor < 5 ||
        session->transport != ADVC_TRANSPORT_DMABUF ||
        session->dmabufs == NULL)
        return ADVC_STATUS_UNSUPPORTED;
    if (advc_dmabuf_unregister_decode(
            request->payload, request->header.payload_size,
            request->header.fd_count, &buffer_id) < 0)
        return dmabuf_errno_status(errno);
    if (advc_dmabuf_registry_unregister(session->dmabufs, buffer_id) < 0)
        return dmabuf_errno_status(errno);
    write_status(reply, ADVC_STATUS_OK, 0);
    return ADVC_STATUS_OK;
}

static uint32_t handle_queue_dmabuf(struct advc_session_engine *engine,
                                    struct advc_session_slot *session,
                                    const struct advc_message *request,
                                    struct advc_message *reply) {
    struct advc_dmabuf_submission submission;
    struct advc_dmabuf_job job;
    struct advc_dmabuf_completion_slot *completion;
    uint32_t status;
    int release_fence = -1;
    if (request->header.version_minor < 5 ||
        session->transport != ADVC_TRANSPORT_DMABUF ||
        session->dmabufs == NULL || engine->ops.submit_dmabuf == NULL)
        return ADVC_STATUS_UNSUPPORTED;
    completion = free_dmabuf_completion(session);
    if (completion == NULL) return ADVC_STATUS_WOULD_BLOCK;
    memset(&submission, 0, sizeof(submission));
    submission.acquire_fence_fd = -1;
    if (advc_dmabuf_submission_decode(
            request->payload, request->header.payload_size, request->fds,
            request->header.fd_count, &submission) < 0)
        return dmabuf_errno_status(errno);
    memset(&job, 0, sizeof(job));
    job.acquire_fence_fd = -1;
    if (advc_dmabuf_registry_begin(session->dmabufs, &submission, &job) < 0)
        return dmabuf_errno_status(errno);
    if (job.descriptor == NULL) {
        (void)advc_dmabuf_registry_finish(session->dmabufs, job.buffer_id);
        advc_dmabuf_job_close(&job);
        return ADVC_STATUS_INTERNAL;
    }
    {
        int acquire_fence = job.acquire_fence_fd;
        job.acquire_fence_fd = -1;
        status = engine->ops.submit_dmabuf(
            engine->backend_userdata, session->backend_handle, job.descriptor,
            job.pts_ns, acquire_fence, &release_fence);
    }
    if (status > ADVC_STATUS_INTERNAL ||
        (status != ADVC_STATUS_OK && release_fence >= 0) ||
        (release_fence >= 0 &&
         advc_dmabuf_sync_file_validate(release_fence) < 0)) {
        if (release_fence >= 0) close(release_fence);
        release_fence = -1;
        status = ADVC_STATUS_INTERNAL;
    }
    if (status != ADVC_STATUS_OK) {
        (void)advc_dmabuf_registry_finish(session->dmabufs, job.buffer_id);
        advc_dmabuf_job_close(&job);
        return status;
    }
    memset(completion, 0, sizeof(*completion));
    completion->buffer_id = job.buffer_id;
    completion->status = ADVC_STATUS_OK;
    completion->release_fence_fd = release_fence;
    completion->in_use = 1;
    advc_dmabuf_job_close(&job);
    write_status(reply, ADVC_STATUS_OK, 0);
    return ADVC_STATUS_OK;
}

static uint32_t handle_complete_dmabuf(struct advc_session_slot *session,
                                       const struct advc_message *request,
                                       struct advc_message *reply) {
    struct advc_dmabuf_completion_slot *completion;
    uint64_t buffer_id;
    int release_fence;
    if (request->header.version_minor < 5 ||
        session->transport != ADVC_TRANSPORT_DMABUF ||
        session->dmabufs == NULL)
        return ADVC_STATUS_UNSUPPORTED;
    if (request->header.payload_size != ADVC_COMPLETE_DMABUF_REQUEST_SIZE ||
        request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    buffer_id = advc_get_u64(
        request->payload + ADVC_COMPLETE_DMABUF_REQUEST_BUFFER_ID_OFFSET);
    if (buffer_id == 0) return ADVC_STATUS_BAD_MESSAGE;
    completion = find_dmabuf_completion(session, buffer_id);
    if (completion == NULL) return ADVC_STATUS_WOULD_BLOCK;
    if (advc_dmabuf_registry_finish(session->dmabufs, buffer_id) < 0)
        return ADVC_STATUS_INTERNAL;
    memset(reply->payload, 0, ADVC_COMPLETE_DMABUF_SIZE);
    advc_put_u64(reply->payload + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET,
                 buffer_id);
    advc_put_u32(reply->payload + ADVC_COMPLETE_DMABUF_STATUS_OFFSET,
                 completion->status);
    advc_put_u32(reply->payload + ADVC_COMPLETE_DMABUF_DETAIL_OFFSET,
                 completion->detail);
    advc_put_u32(reply->payload + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET,
                 completion->release_fence_fd >= 0 ?
                 ADVC_FD_RELEASE_FENCE : ADVC_FD_NONE);
    reply->header.payload_size = ADVC_COMPLETE_DMABUF_SIZE;
    release_fence = completion->release_fence_fd;
    if (release_fence >= 0) {
        reply->header.fd_count = 1;
        reply->fds[0] = release_fence;
    }
    memset(completion, 0, sizeof(*completion));
    completion->release_fence_fd = -1;
    return ADVC_STATUS_OK;
}

static struct advc_output_slot *free_output_slot(struct advc_session_slot *session) {
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        if (!session->outputs[i].in_use) return &session->outputs[i];
    }
    return NULL;
}

static uint32_t handle_dequeue(struct advc_session_engine *engine,
                               struct advc_session_slot *session,
                               const struct advc_message *request,
                               struct advc_message *reply) {
    struct advc_backend_output output;
    struct advc_output_slot *slot;
    uint32_t status;
    int fd;
    if (request->header.payload_size != 0 || request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    slot = free_output_slot(session);
    if (slot == NULL) return ADVC_STATUS_NO_RESOURCE;
    memset(&output, 0, sizeof(output));
    output.acquire_fence_fd = -1;
    status = engine->ops.dequeue_output(engine->backend_userdata, session->backend_handle,
                                        &output);
    if (status != ADVC_STATUS_OK) return status;
    if (output.size > ADVC_MAX_OUTPUT_BYTES || (output.size > 0 && output.data == NULL) ||
        (output.flags & ~(ADVC_FLAG_END_OF_STREAM | ADVC_FLAG_KEY_FRAME |
                          ADVC_FLAG_CODEC_CONFIG)) != 0) {
        engine->ops.release_output(engine->backend_userdata, session->backend_handle,
                                   output.token, -1);
        return ADVC_STATUS_NO_RESOURCE;
    }
    if (output.transport == 0) output.transport = ADVC_TRANSPORT_BYTES;
    if (output.transport == ADVC_TRANSPORT_BYTES) {
        if (output.native_buffer != NULL || output.acquire_fence_fd >= 0) {
            engine->ops.release_output(engine->backend_userdata, session->backend_handle,
                                       output.token, -1);
            if (output.acquire_fence_fd >= 0) close(output.acquire_fence_fd);
            return ADVC_STATUS_INTERNAL;
        }
        fd = create_sealed_output(output.data, output.size);
        engine->ops.release_output(engine->backend_userdata, session->backend_handle,
                                   output.token, -1);
        if (fd < 0) return ADVC_STATUS_INTERNAL;
    } else if (output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        if (output.native_buffer == NULL || output.data != NULL || output.size != 0 ||
            engine->ops.send_native_buffer == NULL) {
            engine->ops.release_output(engine->backend_userdata, session->backend_handle,
                                       output.token, -1);
            if (output.acquire_fence_fd >= 0) close(output.acquire_fence_fd);
            return ADVC_STATUS_UNSUPPORTED;
        }
        fd = -1;
    } else {
        engine->ops.release_output(engine->backend_userdata, session->backend_handle,
                                   output.token, -1);
        if (output.acquire_fence_fd >= 0) close(output.acquire_fence_fd);
        return ADVC_STATUS_UNSUPPORTED;
    }

    memset(reply->payload, 0, ADVC_OUTPUT_BYTES_SIZE);
    slot->id = allocate_output_id(engine);
    slot->in_use = 1;
    advc_put_u64(reply->payload + ADVC_OUTPUT_BUFFER_ID_OFFSET, slot->id);
    advc_put_u64(reply->payload + ADVC_OUTPUT_PTS_NS_OFFSET, output.pts_ns);
    advc_put_u64(reply->payload + ADVC_OUTPUT_SIZE_OFFSET, output.size);
    advc_put_u32(reply->payload + ADVC_OUTPUT_FLAGS_OFFSET, output.flags);
    advc_put_u32(reply->payload + ADVC_OUTPUT_TRANSPORT_OFFSET, output.transport);
    advc_put_u32(reply->payload + ADVC_OUTPUT_WIDTH_OFFSET, output.width);
    advc_put_u32(reply->payload + ADVC_OUTPUT_HEIGHT_OFFSET, output.height);
    advc_put_u32(reply->payload + ADVC_OUTPUT_ANDROID_FORMAT_OFFSET, output.android_format);
    advc_put_u32(reply->payload + ADVC_OUTPUT_STRIDE_OFFSET, output.stride);
    advc_put_u32(reply->payload + ADVC_OUTPUT_LAYERS_OFFSET, output.layers);
    advc_put_u64(reply->payload + ADVC_OUTPUT_USAGE_OFFSET, output.usage);
    advc_put_u32(reply->payload + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET,
                 output.acquire_fence_fd >= 0 ? ADVC_FD_ACQUIRE_FENCE : ADVC_FD_NONE);
    advc_put_u32(reply->payload + ADVC_OUTPUT_SLICE_HEIGHT_OFFSET, output.slice_height);
    advc_put_u32(reply->payload + ADVC_OUTPUT_CROP_LEFT_OFFSET, output.crop_left);
    advc_put_u32(reply->payload + ADVC_OUTPUT_CROP_TOP_OFFSET, output.crop_top);
    advc_put_u32(reply->payload + ADVC_OUTPUT_CROP_RIGHT_OFFSET, output.crop_right);
    advc_put_u32(reply->payload + ADVC_OUTPUT_CROP_BOTTOM_OFFSET, output.crop_bottom);
    reply->header.payload_size = output.transport == ADVC_TRANSPORT_BYTES ?
                                 ADVC_OUTPUT_BYTES_SIZE : ADVC_OUTPUT_READY_SIZE;
    reply->header.fd_count = output.transport == ADVC_TRANSPORT_BYTES ? 1 :
                             (output.acquire_fence_fd >= 0 ? 1 : 0);
    if (fd >= 0) reply->fds[0] = fd;
    else if (output.acquire_fence_fd >= 0) reply->fds[0] = output.acquire_fence_fd;
    if (output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        slot->backend_token = output.token;
        slot->native_buffer = output.native_buffer;
        slot->transport = output.transport;
        slot->prime_metadata.width = output.width;
        slot->prime_metadata.height = output.height;
        slot->prime_metadata.android_format = output.android_format;
        slot->prime_metadata.stride = output.stride;
        slot->prime_metadata.layers = output.layers;
        slot->prime_metadata.usage = output.usage;
        slot->prime_metadata.crop_left = output.crop_left;
        slot->prime_metadata.crop_top = output.crop_top;
        if (output.crop_right >= output.crop_left)
            slot->prime_metadata.crop_width =
                output.crop_right - output.crop_left + 1u;
        if (output.crop_bottom >= output.crop_top)
            slot->prime_metadata.crop_height =
                output.crop_bottom - output.crop_top + 1u;
    }
    return ADVC_STATUS_OK;
}

static uint32_t handle_release(struct advc_session_engine *engine,
                               struct advc_session_slot *session,
                               const struct advc_message *request,
                               struct advc_message *reply) {
    uint64_t id;
    uint32_t fence_role;
    if (request->header.payload_size != ADVC_RELEASE_OUTPUT_SIZE ||
        request->header.fd_count > 1) return ADVC_STATUS_BAD_MESSAGE;
    id = advc_get_u64(request->payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET);
    fence_role = advc_get_u32(request->payload + ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET);
    for (size_t i = ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET + sizeof(uint32_t);
         i < ADVC_RELEASE_OUTPUT_SIZE; ++i) {
        if (request->payload[i] != 0) return ADVC_STATUS_BAD_MESSAGE;
    }
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        if (session->outputs[i].in_use && session->outputs[i].id == id) {
            struct advc_output_slot *slot = &session->outputs[i];
            if (slot->transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
                if ((fence_role == ADVC_FD_NONE && request->header.fd_count != 0) ||
                    (fence_role == ADVC_FD_RELEASE_FENCE && request->header.fd_count != 1) ||
                    (fence_role != ADVC_FD_NONE && fence_role != ADVC_FD_RELEASE_FENCE))
                    return ADVC_STATUS_BAD_MESSAGE;
                int release_fence = request->header.fd_count == 1 ?
                                    fcntl(request->fds[0], F_DUPFD_CLOEXEC, 0) : -1;
                if (request->header.fd_count == 1 && release_fence < 0)
                    return ADVC_STATUS_INTERNAL;
                engine->ops.release_output(engine->backend_userdata,
                                           session->backend_handle,
                                           slot->backend_token,
                                           release_fence);
            } else if (fence_role != ADVC_FD_NONE || request->header.fd_count != 0) {
                return ADVC_STATUS_BAD_MESSAGE;
            }
            memset(&session->outputs[i], 0, sizeof(session->outputs[i]));
            write_status(reply, ADVC_STATUS_OK, 0);
            return ADVC_STATUS_OK;
        }
    }
    return ADVC_STATUS_BAD_MESSAGE;
}

static uint32_t handle_transfer_ahb(struct advc_session_slot *session,
                                    const struct advc_message *request,
                                    struct advc_message *reply) {
    uint64_t id;
    if (request->header.payload_size != ADVC_TRANSFER_AHB_SIZE ||
        request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    id = advc_get_u64(request->payload + ADVC_TRANSFER_AHB_BUFFER_ID_OFFSET);
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        struct advc_output_slot *slot = &session->outputs[i];
        if (!slot->in_use || slot->id != id) continue;
        if (slot->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
            slot->native_buffer == NULL || slot->transferred)
            return ADVC_STATUS_BAD_MESSAGE;
        write_status(reply, ADVC_STATUS_OK, 0);
        reply->header.flags = ADVC_FLAG_AHB_FOLLOWS;
        return ADVC_STATUS_OK;
    }
    return ADVC_STATUS_BAD_MESSAGE;
}

static uint32_t handle_transfer_prime(
    struct advc_session_engine *engine, struct advc_session_slot *session,
    const struct advc_message *request, struct advc_message *reply) {
    struct advc_dmabuf_descriptor descriptor;
    int encoded_fds[ADVC_MAX_DMABUF_OBJECTS];
    uint16_t encoded_fd_count = 0;
    uint64_t id;
    uint32_t status;

    if (request->header.payload_size != ADVC_TRANSFER_PRIME_SIZE ||
        request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    if (session->direction != ADVC_DIRECTION_DECODE ||
        session->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
        engine->ops.export_decode_prime == NULL)
        return ADVC_STATUS_UNSUPPORTED;
    if (reply->payload_capacity < ADVC_TRANSFER_PRIME_REPLY_SIZE)
        return ADVC_STATUS_INTERNAL;
    id = advc_get_u64(request->payload + ADVC_TRANSFER_PRIME_BUFFER_ID_OFFSET);
    if (id == 0) return ADVC_STATUS_BAD_MESSAGE;
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        struct advc_output_slot *slot = &session->outputs[i];
        if (!slot->in_use || slot->id != id) continue;
        if (slot->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
            slot->native_buffer == NULL || slot->prime_transferred)
            return ADVC_STATUS_BAD_MESSAGE;
        memset(&descriptor, 0, sizeof(descriptor));
        for (size_t object = 0; object < ADVC_MAX_DMABUF_OBJECTS; ++object)
            descriptor.objects[object].fd = -1;
        status = engine->ops.export_decode_prime(
            engine->backend_userdata, session->backend_handle,
            slot->native_buffer, &slot->prime_metadata, id, &descriptor);
        if (status > ADVC_STATUS_INTERNAL) status = ADVC_STATUS_INTERNAL;
        if (status != ADVC_STATUS_OK) {
            advc_dmabuf_descriptor_close(&descriptor);
            return status;
        }
        if (descriptor.buffer_id != id ||
            advc_dmabuf_registration_encode(
                reply->payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
                &descriptor, encoded_fds, &encoded_fd_count) < 0 ||
            encoded_fd_count == 0 ||
            encoded_fd_count > ADVC_MAX_DMABUF_OBJECTS) {
            advc_dmabuf_descriptor_close(&descriptor);
            return ADVC_STATUS_INTERNAL;
        }
        advc_put_u32(reply->payload + ADVC_STATUS_CODE_OFFSET,
                     ADVC_STATUS_OK);
        advc_put_u32(reply->payload + ADVC_STATUS_DETAIL_OFFSET, 0);
        reply->header.payload_size = ADVC_TRANSFER_PRIME_REPLY_SIZE;
        reply->header.fd_count = encoded_fd_count;
        for (uint16_t object = 0; object < encoded_fd_count; ++object) {
            reply->fds[object] = encoded_fds[object];
            descriptor.objects[object].fd = -1;
        }
        advc_dmabuf_descriptor_close(&descriptor);
        slot->prime_transferred = 1;
        return ADVC_STATUS_OK;
    }
    return ADVC_STATUS_BAD_MESSAGE;
}

static void release_outstanding(struct advc_session_engine *engine,
                                struct advc_session_slot *session) {
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        struct advc_output_slot *slot = &session->outputs[i];
        if (slot->in_use && slot->transport == ADVC_TRANSPORT_AHARDWAREBUFFER)
            engine->ops.release_output(engine->backend_userdata,
                                       session->backend_handle,
                                       slot->backend_token, -1);
        memset(slot, 0, sizeof(*slot));
    }
    if (session->pending_ahb.acquire_fence_fd >= 0)
        close(session->pending_ahb.acquire_fence_fd);
    if (session->pending_ahb_release_fence >= 0)
        close(session->pending_ahb_release_fence);
    memset(&session->pending_ahb, 0, sizeof(session->pending_ahb));
    session->pending_ahb.acquire_fence_fd = -1;
    session->pending_ahb_release_fence = -1;
    session->pending_ahb_state = 0;
}

static void release_dmabuf_state(struct advc_session_slot *session) {
    for (size_t i = 0; i < ADVC_MAX_INFLIGHT_DMABUFS; ++i) {
        struct advc_dmabuf_completion_slot *completion =
            &session->dmabuf_completions[i];
        if (completion->release_fence_fd >= 0)
            close(completion->release_fence_fd);
        if (completion->in_use && session->dmabufs != NULL)
            (void)advc_dmabuf_registry_finish(
                session->dmabufs, completion->buffer_id);
        memset(completion, 0, sizeof(*completion));
        completion->release_fence_fd = -1;
    }
    advc_dmabuf_registry_destroy(session->dmabufs);
    session->dmabufs = NULL;
}

static uint32_t handle_flush(struct advc_session_engine *engine,
                             struct advc_session_slot *session,
                             const struct advc_message *request,
                             struct advc_message *reply) {
    uint32_t status;
    if (request->header.payload_size != 0 || request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    if (session->transport == ADVC_TRANSPORT_DMABUF)
        return ADVC_STATUS_UNSUPPORTED;
    status = engine->ops.flush(engine->backend_userdata, session->backend_handle);
    if (status == ADVC_STATUS_OK) {
        release_outstanding(engine, session);
        write_status(reply, status, 0);
    }
    return status;
}

static uint32_t handle_close(struct advc_session_engine *engine,
                             struct advc_session_slot *session,
                             const struct advc_message *request,
                             struct advc_message *reply) {
    if (request->header.payload_size != 0 || request->header.fd_count != 0)
        return ADVC_STATUS_BAD_MESSAGE;
    release_outstanding(engine, session);
    release_dmabuf_state(session);
    engine->ops.destroy(engine->backend_userdata, session->backend_handle);
    memset(session, 0, sizeof(*session));
    write_status(reply, ADVC_STATUS_OK, 0);
    return ADVC_STATUS_OK;
}

struct advc_session_engine *advc_session_engine_create(const struct advc_backend_ops *ops,
                                                       void *backend_userdata) {
    struct advc_session_engine *engine;
    if (ops == NULL || ops->create == NULL || ops->queue_input == NULL ||
        ops->dequeue_output == NULL || ops->release_output == NULL ||
        ops->flush == NULL || ops->destroy == NULL) {
        errno = EINVAL;
        return NULL;
    }
    engine = (struct advc_session_engine *)calloc(1, sizeof(*engine));
    if (engine == NULL) return NULL;
    engine->ops = *ops;
    engine->backend_userdata = backend_userdata;
    engine->next_session_id = 1;
    engine->next_output_id = 1;
    return engine;
}

void advc_session_engine_destroy(struct advc_session_engine *engine) {
    if (engine == NULL) return;
    for (size_t i = 0; i < ADVC_MAX_SESSIONS; ++i) {
        if (engine->sessions[i].in_use) {
            release_outstanding(engine, &engine->sessions[i]);
            release_dmabuf_state(&engine->sessions[i]);
            engine->ops.destroy(engine->backend_userdata, engine->sessions[i].backend_handle);
        }
    }
    free(engine);
}

uint32_t advc_session_engine_handle(void *userdata, const struct advc_message *request,
                                    struct advc_message *reply) {
    struct advc_session_engine *engine = (struct advc_session_engine *)userdata;
    struct advc_session_slot *session;
    if (engine == NULL || request == NULL || reply == NULL) return ADVC_STATUS_INTERNAL;
    if (reply->payload == NULL || reply->payload_capacity < ADVC_OUTPUT_BYTES_SIZE ||
        (request->header.payload_size > 0 && request->payload == NULL))
        return ADVC_STATUS_INTERNAL;
    if (request->header.version_minor < 1) return ADVC_STATUS_UNSUPPORTED;
    if (request->header.flags != 0) return ADVC_STATUS_BAD_MESSAGE;
    if (request->header.opcode == ADVC_OP_CREATE_SESSION)
        return handle_create(engine, request, reply);
    session = find_session(engine, request->header.session_id);
    if (session == NULL) return ADVC_STATUS_BAD_MESSAGE;
    if (session->pending_ahb_state != 0 &&
        request->header.opcode != ADVC_OP_COMPLETE_AHB)
        return ADVC_STATUS_WOULD_BLOCK;
    switch (request->header.opcode) {
    case ADVC_OP_QUEUE_INPUT:
        return handle_queue(engine, session, request, reply);
    case ADVC_OP_DEQUEUE_OUTPUT:
        return handle_dequeue(engine, session, request, reply);
    case ADVC_OP_RELEASE_OUTPUT:
        return handle_release(engine, session, request, reply);
    case ADVC_OP_TRANSFER_AHB:
        if (request->header.version_minor < 2) return ADVC_STATUS_UNSUPPORTED;
        return handle_transfer_ahb(session, request, reply);
    case ADVC_OP_TRANSFER_PRIME:
        if (request->header.version_minor < 6) return ADVC_STATUS_UNSUPPORTED;
        return handle_transfer_prime(engine, session, request, reply);
    case ADVC_OP_QUEUE_AHB:
        return handle_queue_ahb(engine, session, request, reply);
    case ADVC_OP_COMPLETE_AHB:
        return handle_complete_ahb(session, request, reply);
    case ADVC_OP_REGISTER_DMABUF:
        return handle_register_dmabuf(session, request, reply);
    case ADVC_OP_UNREGISTER_DMABUF:
        return handle_unregister_dmabuf(session, request, reply);
    case ADVC_OP_QUEUE_DMABUF:
        return handle_queue_dmabuf(engine, session, request, reply);
    case ADVC_OP_COMPLETE_DMABUF:
        return handle_complete_dmabuf(session, request, reply);
    case ADVC_OP_FLUSH:
        return handle_flush(engine, session, request, reply);
    case ADVC_OP_CLOSE_SESSION:
        return handle_close(engine, session, request, reply);
    default:
        return ADVC_STATUS_UNSUPPORTED;
    }
}

int advc_session_engine_after_reply(void *userdata, int client_fd,
                                    const struct advc_message *request,
                                    const struct advc_message *reply) {
    struct advc_session_engine *engine = (struct advc_session_engine *)userdata;
    struct advc_session_slot *session;
    uint64_t id;
    if (engine == NULL || request == NULL || reply == NULL ||
        (reply->header.flags & ADVC_FLAG_AHB_FOLLOWS) == 0)
        return 0;
    session = find_session(engine, request->header.session_id);
    if (session == NULL)
        return -1;
    if (request->header.opcode == ADVC_OP_QUEUE_AHB) {
        struct advc_backend_ahb_input input;
        uint32_t status;
        int release_fence = -1;
        if (session->pending_ahb_state != 1 ||
            engine->ops.receive_ahb_input == NULL)
            return -1;
        input = session->pending_ahb;
        session->pending_ahb.acquire_fence_fd = -1;
        status = engine->ops.receive_ahb_input(
            engine->backend_userdata, session->backend_handle, client_fd,
            &input, &release_fence);
        if (status == ADVC_BACKEND_AHB_FATAL_TRANSPORT) {
            if (release_fence >= 0) close(release_fence);
            memset(&session->pending_ahb, 0, sizeof(session->pending_ahb));
            session->pending_ahb.acquire_fence_fd = -1;
            session->pending_ahb_state = 0;
            return -1;
        }
        if (status > ADVC_STATUS_INTERNAL ||
            (status != ADVC_STATUS_OK && release_fence >= 0)) {
            if (release_fence >= 0) close(release_fence);
            status = ADVC_STATUS_INTERNAL;
            release_fence = -1;
        }
        session->pending_ahb_status = status;
        session->pending_ahb_detail = 0;
        session->pending_ahb_release_fence = release_fence;
        session->pending_ahb_state = 2;
        return 0;
    }
    if (request->header.opcode != ADVC_OP_TRANSFER_AHB ||
        request->header.payload_size != ADVC_TRANSFER_AHB_SIZE)
        return -1;
    id = advc_get_u64(request->payload + ADVC_TRANSFER_AHB_BUFFER_ID_OFFSET);
    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        struct advc_output_slot *slot = &session->outputs[i];
        if (!slot->in_use || slot->id != id) continue;
        if (slot->transferred || slot->native_buffer == NULL ||
            engine->ops.send_native_buffer == NULL)
            return -1;
        if (engine->ops.send_native_buffer(engine->backend_userdata, client_fd,
                                           slot->native_buffer) < 0)
            return -1;
        slot->transferred = 1;
        return 0;
    }
    return -1;
}
