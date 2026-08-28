#define _GNU_SOURCE
#include "advc/byte_range.h"
#include "advc/session_engine.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
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
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

struct mock_session {
    uint8_t input[1024];
    size_t input_size;
    uint64_t input_pts;
    uint32_t input_flags;
    int in_use;
};

struct mock_backend {
    struct mock_session sessions[ADVC_MAX_SESSIONS];
    struct advc_backend_config last_config;
    unsigned creates;
    unsigned queues;
    unsigned dequeues;
    unsigned releases;
    unsigned native_sends;
    unsigned native_receives;
    unsigned prime_exports;
    unsigned dmabuf_checks;
    unsigned dmabuf_submits;
    unsigned release_fences;
    unsigned flushes;
    unsigned destroys;
    uint32_t ahb_receive_status;
    int ahb_return_release_fence;
    int last_ahb_release_fence;
};

#define DRM_FORMAT_ABGR8888 UINT32_C(0x34324241)

static int mock_dmabuf_allowed(
    void *userdata, void *handle,
    const struct advc_dmabuf_descriptor *descriptor) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    assert(handle != NULL && descriptor != NULL);
    ++mock->dmabuf_checks;
    return descriptor->drm_fourcc == DRM_FORMAT_ABGR8888 &&
           descriptor->drm_modifier == 0 && descriptor->object_count == 1 &&
           descriptor->plane_count == 1 &&
           descriptor->planes[0].object_index == 0 &&
           descriptor->planes[0].offset == 0 &&
           descriptor->planes[0].pitch == 128 * 4;
}

static uint32_t mock_submit_dmabuf(
    void *userdata, void *handle,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t pts_ns,
    int acquire_fence_fd, int *release_fence_fd) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    assert(handle != NULL && descriptor != NULL && release_fence_fd != NULL);
    assert(descriptor->buffer_id == 77 && pts_ns == 123456789);
    if (acquire_fence_fd >= 0) close(acquire_fence_fd);
    *release_fence_fd = -1;
    ++mock->dmabuf_submits;
    return ADVC_STATUS_OK;
}

static uint32_t mock_receive_native(void *userdata, void *handle, int socket_fd,
                                    const struct advc_backend_ahb_input *input,
                                    int *release_fence_fd) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    (void)handle;
    (void)socket_fd;
    assert(input != NULL && release_fence_fd != NULL);
    assert(input->width == 128 && input->height == 64 && input->layers == 1);
    if (input->acquire_fence_fd >= 0) close(input->acquire_fence_fd);
    if (mock->ahb_return_release_fence) {
        *release_fence_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(*release_fence_fd >= 0);
        mock->last_ahb_release_fence = *release_fence_fd;
    }
    if (mock->ahb_receive_status != 0)
        return mock->ahb_receive_status;
    if (*release_fence_fd < 0) {
        *release_fence_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(*release_fence_fd >= 0);
        mock->last_ahb_release_fence = *release_fence_fd;
    }
    ++mock->native_receives;
    return ADVC_STATUS_OK;
}

static uint32_t mock_export_decode_prime(
    void *userdata, void *handle, void *native_buffer,
    const struct advc_ahb_public_metadata *metadata, uint64_t buffer_id,
    struct advc_dmabuf_descriptor *descriptor) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    int object_fd;
    assert(handle != NULL && native_buffer == handle && metadata != NULL &&
           descriptor != NULL && buffer_id != 0);
    assert(metadata->width == 1920 && metadata->height == 1080 &&
           metadata->stride == 2048 && metadata->layers == 1 &&
           metadata->crop_left == 0 && metadata->crop_top == 0 &&
           metadata->crop_width == 1920 && metadata->crop_height == 1080);
    object_fd = (int)syscall(SYS_memfd_create, "advc-prime-test", MFD_CLOEXEC);
    assert(object_fd >= 0);
    assert(ftruncate(object_fd, 4 * 1024 * 1024) == 0);
    memset(descriptor, 0, sizeof(*descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
    descriptor->buffer_id = buffer_id;
    descriptor->width = metadata->width;
    descriptor->height = metadata->height;
    descriptor->drm_fourcc = UINT32_C(0x3231564e); /* NV12 */
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->crop_width = metadata->crop_width;
    descriptor->crop_height = metadata->crop_height;
    descriptor->object_count = 1;
    descriptor->plane_count = 2;
    descriptor->objects[0].fd = object_fd;
    descriptor->objects[0].size = 4 * 1024 * 1024;
    descriptor->planes[0].pitch = 2048;
    descriptor->planes[1].offset = 2048 * 1088;
    descriptor->planes[1].pitch = 2048;
    ++mock->prime_exports;
    return ADVC_STATUS_OK;
}

static uint32_t mock_create(void *userdata, const struct advc_backend_config *config,
                            void **handle) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    for (size_t i = 0; i < ADVC_MAX_SESSIONS; ++i) {
        if (!mock->sessions[i].in_use) {
            memset(&mock->sessions[i], 0, sizeof(mock->sessions[i]));
            mock->sessions[i].in_use = 1;
            mock->last_config = *config;
            ++mock->creates;
            *handle = &mock->sessions[i];
            return ADVC_STATUS_OK;
        }
    }
    return ADVC_STATUS_NO_RESOURCE;
}

static uint32_t mock_queue(void *userdata, void *handle, const uint8_t *data,
                           size_t size, uint64_t pts_ns, uint32_t flags) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    struct mock_session *session = (struct mock_session *)handle;
    if (size > sizeof(session->input)) return ADVC_STATUS_NO_RESOURCE;
    if (size > 0) memcpy(session->input, data, size);
    session->input_size = size;
    session->input_pts = pts_ns;
    session->input_flags = flags;
    ++mock->queues;
    return ADVC_STATUS_OK;
}

static uint32_t mock_dequeue(void *userdata, void *handle,
                             struct advc_backend_output *output) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    struct mock_session *session = (struct mock_session *)handle;
    if (session->input_size == 0) return ADVC_STATUS_WOULD_BLOCK;
    memset(output, 0, sizeof(*output));
    output->acquire_fence_fd = -1;
    if (mock->last_config.transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        output->transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
        output->native_buffer = session;
        output->layers = 1;
        output->usage = UINT64_C(0x100);
    } else {
        output->data = session->input;
        output->size = session->input_size;
    }
    output->pts_ns = session->input_pts;
    output->flags = ADVC_FLAG_KEY_FRAME;
    output->width = 1920;
    output->height = 1080;
    output->android_format = 0x15;
    output->stride = 2048;
    output->slice_height = 1088;
    output->crop_right = 1919;
    output->crop_bottom = 1079;
    output->token = ++mock->dequeues;
    return ADVC_STATUS_OK;
}

static void mock_release(void *userdata, void *handle, uintptr_t token,
                         int release_fence_fd) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    (void)handle;
    assert(token > 0);
    if (release_fence_fd >= 0) {
        ++mock->release_fences;
        close(release_fence_fd);
    }
    ++mock->releases;
}

static int mock_send_native(void *userdata, int socket_fd, void *native_buffer) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    const uint8_t marker = 0xa5;
    assert(native_buffer != NULL);
    ++mock->native_sends;
    return send(socket_fd, &marker, sizeof(marker), MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static uint32_t mock_flush(void *userdata, void *handle) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    struct mock_session *session = (struct mock_session *)handle;
    session->input_size = 0;
    ++mock->flushes;
    return ADVC_STATUS_OK;
}

static void mock_destroy(void *userdata, void *handle) {
    struct mock_backend *mock = (struct mock_backend *)userdata;
    struct mock_session *session = (struct mock_session *)handle;
    session->in_use = 0;
    ++mock->destroys;
}

static const struct advc_backend_ops mock_ops = {
    .create = mock_create,
    .queue_input = mock_queue,
    .dequeue_output = mock_dequeue,
    .release_output = mock_release,
    .export_decode_prime = mock_export_decode_prime,
    .send_native_buffer = mock_send_native,
    .receive_ahb_input = mock_receive_native,
    .dmabuf_format_allowed = mock_dmabuf_allowed,
    .submit_dmabuf = mock_submit_dmabuf,
    .flush = mock_flush,
    .destroy = mock_destroy,
};

static void init_message(struct advc_message *message, uint16_t opcode, uint32_t session,
                         uint8_t *payload, uint32_t payload_size, size_t capacity) {
    memset(message, 0, sizeof(*message));
    message->header.version_major = ADVC_VERSION_MAJOR;
    message->header.version_minor = ADVC_VERSION_MINOR;
    message->header.message_type = ADVC_MSG_REQUEST;
    message->header.opcode = opcode;
    message->header.session_id = session;
    message->header.payload_size = payload_size;
    message->payload = payload;
    message->payload_capacity = capacity;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) message->fds[i] = -1;
}

static uint32_t dispatch(struct advc_session_engine *engine, struct advc_message *request,
                         struct advc_message *reply, uint8_t *reply_payload,
                         size_t reply_capacity) {
    memset(reply_payload, 0, reply_capacity);
    init_message(reply, request->header.opcode, request->header.session_id,
                 reply_payload, 0, reply_capacity);
    reply->header.message_type = ADVC_MSG_REPLY;
    return advc_session_engine_handle(engine, request, reply);
}

static uint32_t create_session_transport(struct advc_session_engine *engine,
                               uint8_t direction, uint32_t transport,
                               struct advc_message *reply, uint8_t *reply_payload) {
    uint8_t payload[ADVC_CREATE_SIZE] = {0};
    struct advc_message request;
    advc_put_u32(payload + ADVC_CREATE_DIRECTION_OFFSET, direction);
    advc_put_u32(payload + ADVC_CREATE_WIDTH_OFFSET, 1920);
    advc_put_u32(payload + ADVC_CREATE_HEIGHT_OFFSET, 1080);
    advc_put_u32(payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET, 60000);
    advc_put_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET, transport);
    strcpy((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc");
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, payload, sizeof(payload), sizeof(payload));
    return dispatch(engine, &request, reply, reply_payload, ADVC_OUTPUT_BYTES_SIZE);
}

static uint32_t create_session(struct advc_session_engine *engine, uint8_t direction,
                               struct advc_message *reply, uint8_t *reply_payload) {
    return create_session_transport(engine, direction, ADVC_TRANSPORT_BYTES,
                                    reply, reply_payload);
}

static uint32_t create_encode_session(struct advc_session_engine *engine, const char *mime,
                                      uint32_t width, uint32_t height, uint32_t bitrate,
                                      uint32_t framerate_milli, uint32_t color_format,
                                      struct advc_message *reply, uint8_t *reply_payload) {
    uint8_t payload[ADVC_CREATE_SIZE] = {0};
    struct advc_message request;
    advc_put_u32(payload + ADVC_CREATE_DIRECTION_OFFSET, ADVC_DIRECTION_ENCODE);
    advc_put_u32(payload + ADVC_CREATE_WIDTH_OFFSET, width);
    advc_put_u32(payload + ADVC_CREATE_HEIGHT_OFFSET, height);
    advc_put_u32(payload + ADVC_CREATE_BITRATE_OFFSET, bitrate);
    advc_put_u32(payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET, framerate_milli);
    advc_put_u32(payload + ADVC_CREATE_COLOR_FORMAT_OFFSET, color_format);
    advc_put_u32(payload + ADVC_CREATE_ENCODE_PROFILE_OFFSET,
                 strcmp(mime, "video/hevc") == 0
                     ? ADVC_ENCODE_PROFILE_HEVC_MAIN
                     : ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE);
    strncpy((char *)payload + ADVC_CREATE_MIME_OFFSET, mime, ADVC_MAX_MIME - 1);
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, payload, sizeof(payload), sizeof(payload));
    return dispatch(engine, &request, reply, reply_payload, ADVC_OUTPUT_BYTES_SIZE);
}

static uint32_t create_surface_encode_session(
    struct advc_session_engine *engine, uint32_t color_format,
    struct advc_message *reply, uint8_t *reply_payload) {
    uint8_t payload[ADVC_CREATE_SIZE] = {0};
    struct advc_message request;
    advc_put_u32(payload + ADVC_CREATE_DIRECTION_OFFSET, ADVC_DIRECTION_ENCODE);
    advc_put_u32(payload + ADVC_CREATE_WIDTH_OFFSET, 128);
    advc_put_u32(payload + ADVC_CREATE_HEIGHT_OFFSET, 64);
    advc_put_u32(payload + ADVC_CREATE_BITRATE_OFFSET, 1000000);
    advc_put_u32(payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET, 30000);
    advc_put_u32(payload + ADVC_CREATE_COLOR_FORMAT_OFFSET, color_format);
    advc_put_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET,
                 ADVC_TRANSPORT_BROKER_EGL_SURFACE);
    advc_put_u32(payload + ADVC_CREATE_ENCODE_PROFILE_OFFSET,
                 ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE);
    strcpy((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc");
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, payload, sizeof(payload),
                 sizeof(payload));
    return dispatch(engine, &request, reply, reply_payload,
                    ADVC_OUTPUT_BYTES_SIZE);
}

static uint32_t create_ahb_surface_encode_session(
    struct advc_session_engine *engine, uint16_t minor,
    struct advc_message *reply, uint8_t *reply_payload) {
    uint8_t payload[ADVC_CREATE_SIZE] = {0};
    struct advc_message request;
    advc_put_u32(payload + ADVC_CREATE_DIRECTION_OFFSET, ADVC_DIRECTION_ENCODE);
    advc_put_u32(payload + ADVC_CREATE_WIDTH_OFFSET, 128);
    advc_put_u32(payload + ADVC_CREATE_HEIGHT_OFFSET, 64);
    advc_put_u32(payload + ADVC_CREATE_BITRATE_OFFSET, 1000000);
    advc_put_u32(payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET, 30000);
    advc_put_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET,
                 ADVC_TRANSPORT_ANDROID_AHB_SURFACE);
    advc_put_u32(payload + ADVC_CREATE_ENCODE_PROFILE_OFFSET,
                 ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE);
    strcpy((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc");
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, payload, sizeof(payload),
                 sizeof(payload));
    request.header.version_minor = minor;
    return dispatch(engine, &request, reply, reply_payload,
                    ADVC_OUTPUT_BYTES_SIZE);
}

static void make_dmabuf_registration(uint8_t payload[ADVC_REGISTER_DMABUF_SIZE],
                                     uint64_t object_size) {
    memset(payload, 0, ADVC_REGISTER_DMABUF_SIZE);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET, 77);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_WIDTH_OFFSET, 128);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_HEIGHT_OFFSET, 64);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FOURCC_OFFSET,
                 DRM_FORMAT_ABGR8888);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_FLAGS_OFFSET,
                 ADVC_DMABUF_EXPLICIT_ALL);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET, 0);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET, 128);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_CROP_HEIGHT_OFFSET, 64);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET, 1);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET, 1);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET,
                 ADVC_COLOR_MATRIX_RGB);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_COLOR_RANGE_OFFSET,
                 ADVC_COLOR_RANGE_FULL);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                 ADVC_REGISTER_DMABUF_OBJECT_FD_INDEX_OFFSET, 0);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_OBJECTS_OFFSET +
                 ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET, object_size);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET, 0);
    advc_put_u64(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET, 0);
    advc_put_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                 ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET, 128 * 4);
}

static int create_input_memfd(const uint8_t *data, size_t size, int sealed) {
    int fd;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "advc-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    fd = (int)syscall(__NR_memfd_create, "advc-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    if (size > 0) assert(pwrite(fd, data, size, 0) == (ssize_t)size);
    if (sealed)
        assert(fcntl(fd, F_ADD_SEALS,
                     F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0);
    return fd;
}

static void test_complete_lifecycle(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t queue_payload[ADVC_QUEUE_INPUT_SIZE + 4] = {0};
    uint8_t release_payload[ADVC_RELEASE_OUTPUT_SIZE] = {0};
    uint32_t session_id;
    uint64_t output_id;
    char output[4];
    assert(engine != NULL);

    assert(create_session(engine, ADVC_DIRECTION_DECODE, &reply, reply_payload) == ADVC_STATUS_OK);
    assert(advc_get_u32(reply_payload) == ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    assert(session_id != 0);
    assert(strcmp(mock.last_config.mime, "video/avc") == 0);
    assert(mock.last_config.width == 1920 && mock.last_config.framerate_milli == 60000);

    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_PTS_NS_OFFSET, 123000);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_DATA_OFFSET, ADVC_QUEUE_INPUT_SIZE);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 4);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET, ADVC_FLAG_KEY_FRAME);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);
    memcpy(queue_payload + ADVC_QUEUE_INPUT_SIZE, "test", 4);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(mock.queues == 1 && mock.sessions[0].input_pts == 123000);

    init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(reply.header.fd_count == 1);
    assert(advc_get_u32(reply_payload + ADVC_OUTPUT_TRANSPORT_OFFSET) == ADVC_TRANSPORT_BYTES);
    assert(advc_get_u64(reply_payload + ADVC_OUTPUT_SIZE_OFFSET) == 4);
    assert(reply.header.payload_size == ADVC_OUTPUT_BYTES_SIZE);
    assert(advc_get_u32(reply_payload + ADVC_OUTPUT_STRIDE_OFFSET) == 2048);
    assert(advc_get_u32(reply_payload + ADVC_OUTPUT_SLICE_HEIGHT_OFFSET) == 1088);
    assert(pread(reply.fds[0], output, sizeof(output), 0) == (ssize_t)sizeof(output));
    assert(memcmp(output, "test", 4) == 0);
    assert((fcntl(reply.fds[0], F_GET_SEALS) &
            (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) ==
           (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL));
    output_id = advc_get_u64(reply_payload + ADVC_OUTPUT_BUFFER_ID_OFFSET);
    close(reply.fds[0]);
    assert(mock.releases == 1);

    advc_put_u64(release_payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET, output_id);
    advc_put_u32(release_payload + ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_RELEASE_OUTPUT, session_id, release_payload,
                 sizeof(release_payload), sizeof(release_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);

    init_message(&request, ADVC_OP_FLUSH, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(mock.flushes == 1);
    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(mock.destroys == 1);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE);
    advc_session_engine_destroy(engine);
}

static void test_encode_lifecycle_and_validation(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t queue_payload[ADVC_QUEUE_INPUT_SIZE + 384] = {0};
    uint32_t session_id;
    assert(engine != NULL);

    assert(create_encode_session(engine, "video/avc", 16, 16, 1000000, 30000,
                                 ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    assert(mock.last_config.direction == ADVC_DIRECTION_ENCODE);
    assert(mock.last_config.bitrate == 1000000);
    assert(mock.last_config.color_format == ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR);

    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_DATA_OFFSET, ADVC_QUEUE_INPUT_SIZE);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 383);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 ADVC_QUEUE_INPUT_SIZE + 383, sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE);
    assert(mock.queues == 0);

    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 384);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(mock.queues == 1 && mock.sessions[0].input_size == 384);

    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET, ADVC_FLAG_KEY_FRAME);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET, ADVC_FLAG_END_OF_STREAM);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 0);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 ADVC_QUEUE_INPUT_SIZE, sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);

    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(create_encode_session(engine, "video/hevc", 16, 16, 1000000, 30000,
                                 ADVC_COLOR_FORMAT_YUV420_PLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);

    assert(create_encode_session(engine, "video/x-vnd.on2.vp9", 16, 16, 1000000,
                                 30000, ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_BAD_MESSAGE);
    assert(create_encode_session(engine, "video/avc", 17, 16, 1000000, 30000,
                                 ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_BAD_MESSAGE);
    assert(create_encode_session(engine, "video/avc", 16, 16, 0, 30000,
                                 ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_BAD_MESSAGE);
    assert(create_encode_session(engine, "video/avc", 16, 16, 1000000, 0,
                                 ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_BAD_MESSAGE);
    assert(create_encode_session(engine, "video/avc", 16, 16, 1000000, 30000,
                                 0x7f420888u, &reply, reply_payload) ==
           ADVC_STATUS_BAD_MESSAGE);
    assert(create_encode_session(engine, "video/avc", 8192, 8192, 1000000, 30000,
                                 ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR,
                                 &reply, reply_payload) == ADVC_STATUS_NO_RESOURCE);
    advc_session_engine_destroy(engine);
}

static void test_surface_encode_control_frames(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t queue_payload[ADVC_QUEUE_INPUT_SIZE] = {0};
    uint32_t session_id;
    int empty_fd;
    assert(engine != NULL);
    assert(create_surface_encode_session(engine, 0, &reply, reply_payload) ==
           ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    assert(mock.last_config.transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE);
    assert(mock.last_config.color_format == 0);

    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_PTS_NS_OFFSET, 123456);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_DATA_OFFSET,
                 ADVC_QUEUE_INPUT_SIZE);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(mock.queues == 1 && mock.sessions[0].input_size == 0);
    assert(mock.sessions[0].input_pts == 123456);

    empty_fd = create_input_memfd(NULL, 0, 1);
    assert(empty_fd >= 0);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET,
                 ADVC_FD_INPUT_DATA);
    request.header.fd_count = 1;
    request.fds[0] = empty_fd;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_BAD_MESSAGE);
    close(empty_fd);
    request.header.fd_count = 0;
    request.fds[0] = -1;
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);

    queue_payload[ADVC_QUEUE_INPUT_SIZE - 1] = 1;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_BAD_MESSAGE);
    queue_payload[ADVC_QUEUE_INPUT_SIZE - 1] = 0;
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET,
                 ADVC_FLAG_END_OF_STREAM);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);

    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(create_surface_encode_session(engine, ADVC_COLOR_FORMAT_YUV420_PLANAR,
                                         &reply, reply_payload) ==
           ADVC_STATUS_BAD_MESSAGE);
    advc_session_engine_destroy(engine);
}

static void test_android_ahb_surface_handshake(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t queue[ADVC_QUEUE_AHB_SIZE] = {0};
    uint32_t session_id;
    int acquire_fence;
    assert(engine != NULL);
    assert(create_ahb_surface_encode_session(engine, 6, &reply, reply_payload) ==
           ADVC_STATUS_UNSUPPORTED);
    assert(create_ahb_surface_encode_session(engine, 7, &reply, reply_payload) ==
           ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    advc_put_u64(queue + ADVC_QUEUE_AHB_PTS_NS_OFFSET, 123456);
    advc_put_u32(queue + ADVC_QUEUE_AHB_WIDTH_OFFSET, 128);
    advc_put_u32(queue + ADVC_QUEUE_AHB_HEIGHT_OFFSET, 64);
    advc_put_u32(queue + ADVC_QUEUE_AHB_FORMAT_OFFSET, 1);
    advc_put_u32(queue + ADVC_QUEUE_AHB_LAYERS_OFFSET, 1);
    advc_put_u64(queue + ADVC_QUEUE_AHB_USAGE_OFFSET, UINT64_C(0x100));
    advc_put_u32(queue + ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET,
                 ADVC_FD_ACQUIRE_FENCE);
    init_message(&request, ADVC_OP_QUEUE_AHB, session_id, queue, sizeof(queue),
                 sizeof(queue));
    acquire_fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire_fence >= 0);
    request.header.fd_count = 1;
    request.fds[0] = acquire_fence;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(reply.header.flags == ADVC_FLAG_AHB_FOLLOWS);
    assert(advc_session_engine_after_reply(engine, -1, &request, &reply) == 0);
    close(acquire_fence);
    assert(mock.native_receives == 1);
    init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_WOULD_BLOCK);
    init_message(&request, ADVC_OP_QUEUE_AHB, session_id, queue, sizeof(queue),
                 sizeof(queue));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_WOULD_BLOCK);
    init_message(&request, ADVC_OP_COMPLETE_AHB, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(reply.header.payload_size == ADVC_COMPLETE_AHB_SIZE);
    assert(advc_get_u32(reply_payload + ADVC_COMPLETE_AHB_STATUS_OFFSET) ==
           ADVC_STATUS_OK);
    assert(advc_get_u32(reply_payload + ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET) ==
           ADVC_FD_RELEASE_FENCE);
    assert(reply.header.fd_count == 1 && reply.fds[0] >= 0);
    close(reply.fds[0]);

    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_WOULD_BLOCK);
    advc_session_engine_destroy(engine);

    memset(&mock, 0, sizeof(mock));
    mock.ahb_receive_status = ADVC_BACKEND_AHB_FATAL_TRANSPORT;
    mock.ahb_return_release_fence = 1;
    engine = advc_session_engine_create(&mock_ops, &mock);
    assert(engine != NULL);
    assert(create_ahb_surface_encode_session(engine, 7, &reply, reply_payload) ==
           ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    memset(queue, 0, sizeof(queue));
    advc_put_u64(queue + ADVC_QUEUE_AHB_PTS_NS_OFFSET, 1);
    advc_put_u32(queue + ADVC_QUEUE_AHB_WIDTH_OFFSET, 128);
    advc_put_u32(queue + ADVC_QUEUE_AHB_HEIGHT_OFFSET, 64);
    advc_put_u32(queue + ADVC_QUEUE_AHB_FORMAT_OFFSET, 1);
    advc_put_u32(queue + ADVC_QUEUE_AHB_LAYERS_OFFSET, 1);
    advc_put_u64(queue + ADVC_QUEUE_AHB_USAGE_OFFSET, UINT64_C(0x100));
    advc_put_u32(queue + ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_QUEUE_AHB, session_id, queue, sizeof(queue),
                 sizeof(queue));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(advc_session_engine_after_reply(engine, -1, &request, &reply) == -1);
    errno = 0;
    assert(fcntl(mock.last_ahb_release_fence, F_GETFD) == -1 && errno == EBADF);
    init_message(&request, ADVC_OP_COMPLETE_AHB, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_WOULD_BLOCK);
    advc_session_engine_destroy(engine);

    memset(&mock, 0, sizeof(mock));
    engine = advc_session_engine_create(&mock_ops, &mock);
    assert(engine != NULL);
    assert(create_ahb_surface_encode_session(engine, 7, &reply, reply_payload) ==
           ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    init_message(&request, ADVC_OP_QUEUE_AHB, session_id, queue, sizeof(queue),
                 sizeof(queue));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(advc_session_engine_after_reply(engine, -1, &request, &reply) == 0);
    assert(mock.last_ahb_release_fence >= 0);
    advc_session_engine_destroy(engine);
    errno = 0;
    assert(fcntl(mock.last_ahb_release_fence, F_GETFD) == -1 && errno == EBADF);
}

static void test_memfd_and_resource_bounds(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t queue_payload[ADVC_QUEUE_INPUT_SIZE] = {0};
    uint8_t release_payload[ADVC_RELEASE_OUTPUT_SIZE] = {0};
    uint32_t sessions[ADVC_MAX_SESSIONS];
    uint64_t output_ids[ADVC_MAX_OUTSTANDING_OUTPUTS];
    int fd;
    assert(engine != NULL);
    for (size_t i = 0; i < ADVC_MAX_SESSIONS; ++i) {
        assert(create_session(engine, ADVC_DIRECTION_DECODE, &reply, reply_payload) ==
               ADVC_STATUS_OK);
        sessions[i] = reply.header.session_id;
    }
    assert(create_session(engine, ADVC_DIRECTION_DECODE, &reply, reply_payload) ==
           ADVC_STATUS_NO_RESOURCE);

    fd = create_input_memfd((const uint8_t *)"memfd", 5, 1);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_DATA_OFFSET, 0);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 5);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_INPUT_DATA);
    init_message(&request, ADVC_OP_QUEUE_INPUT, sessions[0], queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    request.header.fd_count = 1;
    request.fds[0] = fd;
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    close(fd);
    assert(mock.sessions[0].input_size == 5);
    assert(memcmp(mock.sessions[0].input, "memfd", 5) == 0);

    for (size_t i = 0; i < ADVC_MAX_OUTSTANDING_OUTPUTS; ++i) {
        init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, sessions[0], NULL, 0, 0);
        assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
               ADVC_STATUS_OK);
        output_ids[i] = advc_get_u64(reply_payload + ADVC_OUTPUT_BUFFER_ID_OFFSET);
        close(reply.fds[0]);
    }
    init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, sessions[0], NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_NO_RESOURCE);
    advc_put_u64(release_payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET, output_ids[0]);
    init_message(&request, ADVC_OP_RELEASE_OUTPUT, sessions[0], release_payload,
                 sizeof(release_payload), sizeof(release_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, sessions[0], NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    close(reply.fds[0]);

    fd = create_input_memfd((const uint8_t *)"bad", 3, 0);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 3);
    init_message(&request, ADVC_OP_QUEUE_INPUT, sessions[0], queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    request.header.fd_count = 1;
    request.fds[0] = fd;
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE);
    close(fd);
    advc_session_engine_destroy(engine);
    assert(mock.destroys == ADVC_MAX_SESSIONS);
}

static void test_minor_version_rejection(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t payload[ADVC_CREATE_SIZE] = {0};
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, payload, sizeof(payload), sizeof(payload));
    request.header.version_minor = 0;
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_UNSUPPORTED);
    request.header.version_minor = 1;
    advc_put_u32(payload + ADVC_CREATE_DIRECTION_OFFSET, ADVC_DIRECTION_DECODE);
    advc_put_u32(payload + ADVC_CREATE_WIDTH_OFFSET, 1920);
    advc_put_u32(payload + ADVC_CREATE_HEIGHT_OFFSET, 1080);
    advc_put_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET,
                 ADVC_TRANSPORT_AHARDWAREBUFFER);
    strcpy((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc");
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_UNSUPPORTED);
    /* Minor 1 with transport zero remains the legacy byte contract. */
    advc_put_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    advc_session_engine_destroy(engine);
}

static void test_ahb_transfer_and_fence_lifecycle(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_TRANSFER_PRIME_REPLY_SIZE];
    uint8_t queue_payload[ADVC_QUEUE_INPUT_SIZE + 4] = {0};
    uint8_t transfer_payload[ADVC_TRANSFER_AHB_SIZE] = {0};
    uint8_t prime_payload[ADVC_TRANSFER_PRIME_SIZE] = {0};
    uint8_t release_payload[ADVC_RELEASE_OUTPUT_SIZE] = {0};
    uint32_t session_id;
    uint64_t output_id;
    uint8_t marker = 0;
    int sockets[2];
    int fence;
    assert(engine != NULL);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(create_session_transport(engine, ADVC_DIRECTION_DECODE,
                                    ADVC_TRANSPORT_AHARDWAREBUFFER,
                                    &reply, reply_payload) == ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    assert(mock.last_config.transport == ADVC_TRANSPORT_AHARDWAREBUFFER);

    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_DATA_OFFSET, ADVC_QUEUE_INPUT_SIZE);
    advc_put_u64(queue_payload + ADVC_QUEUE_INPUT_SIZE_OFFSET, 4);
    advc_put_u32(queue_payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);
    memcpy(queue_payload + ADVC_QUEUE_INPUT_SIZE, "ahb!", 4);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, queue_payload,
                 sizeof(queue_payload), sizeof(queue_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);

    init_message(&request, ADVC_OP_DEQUEUE_OUTPUT, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(reply.header.payload_size == ADVC_OUTPUT_READY_SIZE);
    assert(reply.header.fd_count == 0);
    assert(advc_get_u32(reply_payload + ADVC_OUTPUT_TRANSPORT_OFFSET) ==
           ADVC_TRANSPORT_AHARDWAREBUFFER);
    assert(advc_get_u64(reply_payload + ADVC_OUTPUT_SIZE_OFFSET) == 0);
    output_id = advc_get_u64(reply_payload + ADVC_OUTPUT_BUFFER_ID_OFFSET);
    assert(mock.releases == 0); /* AImage is retained until RELEASE_OUTPUT. */

    advc_put_u64(prime_payload, output_id);
    init_message(&request, ADVC_OP_TRANSFER_PRIME, session_id, prime_payload,
                 sizeof(prime_payload), sizeof(prime_payload));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(reply.header.payload_size == ADVC_TRANSFER_PRIME_REPLY_SIZE);
    assert(reply.header.fd_count == 1 && reply.fds[0] >= 0);
    assert(advc_get_u32(reply_payload + ADVC_STATUS_CODE_OFFSET) ==
           ADVC_STATUS_OK);
    assert(advc_get_u64(reply_payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET +
                        ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET) == output_id);
    assert(advc_get_u32(reply_payload + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET +
                        ADVC_REGISTER_DMABUF_FOURCC_OFFSET) ==
           UINT32_C(0x3231564e));
    close(reply.fds[0]);
    reply.fds[0] = -1;
    assert(mock.prime_exports == 1);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_BAD_MESSAGE);

    advc_put_u64(transfer_payload, output_id);
    init_message(&request, ADVC_OP_TRANSFER_AHB, session_id, transfer_payload,
                 sizeof(transfer_payload), sizeof(transfer_payload));
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(reply.header.flags == ADVC_FLAG_AHB_FOLLOWS);
    assert(advc_session_engine_after_reply(engine, sockets[0], &request, &reply) == 0);
    assert(recv(sockets[1], &marker, 1, 0) == 1 && marker == 0xa5);
    assert(mock.native_sends == 1);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_BAD_MESSAGE); /* Exactly one native-handle transfer. */

    fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fence >= 0);
    advc_put_u64(release_payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET, output_id);
    advc_put_u32(release_payload + ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET,
                 ADVC_FD_RELEASE_FENCE);
    init_message(&request, ADVC_OP_RELEASE_OUTPUT, session_id, release_payload,
                 sizeof(release_payload), sizeof(release_payload));
    request.header.fd_count = 1;
    request.fds[0] = fence;
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    assert(mock.releases == 1 && mock.release_fences == 1);
    assert(fcntl(fence, F_GETFD) >= 0); /* Engine duplicates; caller retains its fd. */
    close(fence);

    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload, sizeof(reply_payload)) ==
           ADVC_STATUS_OK);
    close(sockets[0]);
    close(sockets[1]);
    advc_session_engine_destroy(engine);
}

static void test_checked_output_ranges(void) {
    uint8_t buffer[16] = {0};
    const uint8_t *data = NULL;
    size_t size = 0;
    assert(advc_checked_byte_range(buffer, sizeof(buffer), 3, 5, &data, &size) == 0);
    assert(data == buffer + 3 && size == 5);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), 16, 0, &data, &size) == 0);
    assert(data == buffer + 16 && size == 0);
    assert(advc_checked_byte_range(NULL, 0, 0, 0, &data, &size) == 0);
    assert(data == NULL && size == 0);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), -1, 1, &data, &size) < 0);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), 0, -1, &data, &size) < 0);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), 17, 0, &data, &size) < 0);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), 15, 2, &data, &size) < 0);
    assert(advc_checked_byte_range(buffer, sizeof(buffer), INT64_MAX, INT64_MAX,
                                   &data, &size) < 0);
    assert(advc_checked_byte_range(NULL, 8, 0, 1, &data, &size) < 0);
}

static void test_dmabuf_encode_lifecycle(void) {
    struct mock_backend mock = {0};
    struct advc_session_engine *engine = advc_session_engine_create(&mock_ops, &mock);
    struct advc_message request;
    struct advc_message reply;
    uint8_t reply_payload[ADVC_OUTPUT_BYTES_SIZE];
    uint8_t create[ADVC_CREATE_SIZE] = {0};
    uint8_t registration[ADVC_REGISTER_DMABUF_SIZE];
    uint8_t queue[ADVC_QUEUE_DMABUF_SIZE] = {0};
    uint8_t complete[ADVC_COMPLETE_DMABUF_REQUEST_SIZE] = {0};
    uint8_t unregister_payload[ADVC_UNREGISTER_DMABUF_SIZE] = {0};
    uint8_t eos[ADVC_QUEUE_INPUT_SIZE] = {0};
    uint32_t session_id;
    int object_fd;

    assert(engine != NULL);
    advc_put_u32(create + ADVC_CREATE_DIRECTION_OFFSET, ADVC_DIRECTION_ENCODE);
    advc_put_u32(create + ADVC_CREATE_WIDTH_OFFSET, 128);
    advc_put_u32(create + ADVC_CREATE_HEIGHT_OFFSET, 64);
    advc_put_u32(create + ADVC_CREATE_BITRATE_OFFSET, 1000000);
    advc_put_u32(create + ADVC_CREATE_FRAMERATE_MILLI_OFFSET, 30000);
    advc_put_u32(create + ADVC_CREATE_TRANSPORT_OFFSET, ADVC_TRANSPORT_DMABUF);
    advc_put_u32(create + ADVC_CREATE_ENCODE_PROFILE_OFFSET,
                 ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE);
    strcpy((char *)create + ADVC_CREATE_MIME_OFFSET, "video/avc");
    init_message(&request, ADVC_OP_CREATE_SESSION, 0, create, sizeof(create),
                 sizeof(create));
    request.header.version_minor = 6;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_UNSUPPORTED);
    request.header.version_minor = 7;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    session_id = reply.header.session_id;
    assert(mock.creates == 1 && mock.last_config.transport == ADVC_TRANSPORT_DMABUF);

    object_fd = memfd_create("advc-session-dmabuf", MFD_CLOEXEC);
    assert(object_fd >= 0);
    assert(ftruncate(object_fd, 128 * 64 * 4) == 0);
    make_dmabuf_registration(registration, 128 * 64 * 4);
    init_message(&request, ADVC_OP_REGISTER_DMABUF, session_id, registration,
                 sizeof(registration), sizeof(registration));
    request.header.fd_count = 1;
    request.fds[0] = object_fd;
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(mock.dmabuf_checks == 1);

    advc_put_u64(queue + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET, 77);
    advc_put_u64(queue + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET, 123456789);
    advc_put_u32(queue + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_QUEUE_DMABUF, session_id, queue,
                 sizeof(queue), sizeof(queue));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(mock.dmabuf_submits == 1);

    advc_put_u64(unregister_payload + ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET, 77);
    init_message(&request, ADVC_OP_UNREGISTER_DMABUF, session_id,
                 unregister_payload, sizeof(unregister_payload),
                 sizeof(unregister_payload));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_BAD_MESSAGE);

    advc_put_u64(complete + ADVC_COMPLETE_DMABUF_REQUEST_BUFFER_ID_OFFSET, 77);
    init_message(&request, ADVC_OP_COMPLETE_DMABUF, session_id, complete,
                 sizeof(complete), sizeof(complete));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(reply.header.payload_size == ADVC_COMPLETE_DMABUF_SIZE);
    assert(reply.header.fd_count == 0);
    assert(advc_get_u64(reply_payload + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET) == 77);
    assert(advc_get_u32(reply_payload + ADVC_COMPLETE_DMABUF_STATUS_OFFSET) ==
           ADVC_STATUS_OK);
    assert(advc_get_u32(reply_payload + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET) ==
           ADVC_FD_NONE);

    init_message(&request, ADVC_OP_UNREGISTER_DMABUF, session_id,
                 unregister_payload, sizeof(unregister_payload),
                 sizeof(unregister_payload));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);

    advc_put_u64(eos + ADVC_QUEUE_INPUT_DATA_OFFSET, ADVC_QUEUE_INPUT_SIZE);
    advc_put_u32(eos + ADVC_QUEUE_INPUT_FLAGS_OFFSET, ADVC_FLAG_END_OF_STREAM);
    advc_put_u32(eos + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET, ADVC_FD_NONE);
    init_message(&request, ADVC_OP_QUEUE_INPUT, session_id, eos, sizeof(eos),
                 sizeof(eos));
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    assert(mock.queues == 1);

    init_message(&request, ADVC_OP_CLOSE_SESSION, session_id, NULL, 0, 0);
    assert(dispatch(engine, &request, &reply, reply_payload,
                    sizeof(reply_payload)) == ADVC_STATUS_OK);
    close(object_fd);
    advc_session_engine_destroy(engine);

    {
        struct advc_backend_ops no_dmabuf = mock_ops;
        no_dmabuf.dmabuf_format_allowed = NULL;
        no_dmabuf.submit_dmabuf = NULL;
        memset(&mock, 0, sizeof(mock));
        engine = advc_session_engine_create(&no_dmabuf, &mock);
        assert(engine != NULL);
        init_message(&request, ADVC_OP_CREATE_SESSION, 0, create, sizeof(create),
                     sizeof(create));
        assert(dispatch(engine, &request, &reply, reply_payload,
                        sizeof(reply_payload)) == ADVC_STATUS_UNSUPPORTED);
        assert(mock.creates == 0);
        advc_session_engine_destroy(engine);
    }
}

int main(void) {
    test_android_ahb_surface_handshake();
    test_surface_encode_control_frames();
    test_complete_lifecycle();
    test_encode_lifecycle_and_validation();
    test_memfd_and_resource_bounds();
    test_minor_version_rejection();
    test_ahb_transfer_and_fence_lifecycle();
    test_dmabuf_encode_lifecycle();
    test_checked_output_ranges();
    puts("session_engine_test: all tests passed");
    return 0;
}
