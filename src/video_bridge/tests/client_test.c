#define _GNU_SOURCE
#include "advc/broker.h"
#include "advc/client.h"
#include "advc_annexb.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif

static const uint8_t avc_sample[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
    0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
    0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
};
#define AVC_CONFIG_SIZE 15u
#define AVC_FRAME_OFFSET 15u
#define AVC_FRAME_SIZE (sizeof(avc_sample) - AVC_FRAME_OFFSET)
static size_t fd_count(void);
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002u
#endif

static int sealed_memfd(const uint8_t *data, size_t size) {
    int fd = (int)syscall(SYS_memfd_create, "advc-client-test",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(fd >= 0);
    if (size > 0) assert(write(fd, data, size) == (ssize_t)size);
    assert(fcntl(fd, F_ADD_SEALS,
                 F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0);
    return fd;
}

static void receive_request(int fd, struct advc_message *request, uint8_t *payload) {
    memset(request, 0, sizeof(*request));
    request->payload = payload;
    request->payload_capacity = ADVC_MAX_PAYLOAD;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) request->fds[i] = -1;
    assert(advc_receive_message(fd, request) == 0);
    assert(request->header.version_major == ADVC_VERSION_MAJOR);
    assert(request->header.version_minor == ADVC_VERSION_MINOR);
    assert(request->header.message_type == ADVC_MSG_REQUEST);
}

static void send_status(int fd, const struct advc_message *request, uint32_t session_id,
                        uint32_t status, int attached_fd) {
    uint8_t payload[ADVC_STATUS_SIZE] = {0};
    struct advc_message reply;
    memset(&reply, 0, sizeof(reply));
    reply.header = request->header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.session_id = session_id;
    reply.header.payload_size = sizeof(payload);
    advc_put_u32(payload + ADVC_STATUS_CODE_OFFSET, status);
    reply.payload = payload;
    if (attached_fd >= 0) {
        reply.header.fd_count = 1;
        reply.fds[0] = attached_fd;
    }
    assert(advc_send_message(fd, &reply) == 0);
}

static void run_happy_server(int fd) {
    static uint8_t payload[ADVC_MAX_PAYLOAD];
    static const uint8_t output_bytes[] = {1, 2, 3, 4, 5, 6};
    struct advc_message request;
    uint8_t output[ADVC_OUTPUT_BYTES_SIZE] = {0};
    uint8_t readback[sizeof(avc_sample)];
    int output_fd;

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CREATE_SESSION);
    assert(request.header.session_id == 0 && request.header.fd_count == 0);
    assert(request.header.payload_size == ADVC_CREATE_SIZE);
    assert(advc_get_u32(payload + ADVC_CREATE_DIRECTION_OFFSET) == ADVC_DIRECTION_DECODE);
    assert(advc_get_u32(payload + ADVC_CREATE_WIDTH_OFFSET) == 1920);
    assert(advc_get_u32(payload + ADVC_CREATE_HEIGHT_OFFSET) == 1080);
    assert(strcmp((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc") == 0);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_INPUT && request.header.session_id == 11);
    assert(request.header.fd_count == 1 && request.header.payload_size == ADVC_QUEUE_INPUT_SIZE);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FD_ROLE_OFFSET) == ADVC_FD_INPUT_DATA);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_DATA_OFFSET) == 0);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) == AVC_CONFIG_SIZE);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) == ADVC_FLAG_CODEC_CONFIG);
    assert(pread(request.fds[0], readback, AVC_CONFIG_SIZE, 0) == AVC_CONFIG_SIZE);
    assert(memcmp(readback, avc_sample, AVC_CONFIG_SIZE) == 0);
    advc_close_message_fds(&request);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_INPUT && request.header.fd_count == 1);
    assert(request.header.payload_size == ADVC_QUEUE_INPUT_SIZE);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_DATA_OFFSET) == AVC_FRAME_OFFSET);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) == AVC_FRAME_SIZE);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) == ADVC_FLAG_KEY_FRAME);
    assert(pread(request.fds[0], readback, AVC_FRAME_SIZE, AVC_FRAME_OFFSET) == AVC_FRAME_SIZE);
    assert(memcmp(readback, avc_sample + AVC_FRAME_OFFSET, AVC_FRAME_SIZE) == 0);
    advc_close_message_fds(&request);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_INPUT && request.header.fd_count == 0);
    assert(request.header.payload_size == ADVC_QUEUE_INPUT_SIZE);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) == 0);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) == ADVC_FLAG_END_OF_STREAM);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_DEQUEUE_OUTPUT && request.header.payload_size == 0);
    send_status(fd, &request, 11, ADVC_STATUS_WOULD_BLOCK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_DEQUEUE_OUTPUT && request.header.payload_size == 0);
    output_fd = sealed_memfd(output_bytes, sizeof(output_bytes));
    advc_put_u64(output + ADVC_OUTPUT_BUFFER_ID_OFFSET, 77);
    advc_put_u64(output + ADVC_OUTPUT_PTS_NS_OFFSET, 123456);
    advc_put_u64(output + ADVC_OUTPUT_SIZE_OFFSET, sizeof(output_bytes));
    advc_put_u32(output + ADVC_OUTPUT_FLAGS_OFFSET, ADVC_FLAG_KEY_FRAME);
    advc_put_u32(output + ADVC_OUTPUT_TRANSPORT_OFFSET, ADVC_TRANSPORT_BYTES);
    advc_put_u32(output + ADVC_OUTPUT_WIDTH_OFFSET, 1920);
    advc_put_u32(output + ADVC_OUTPUT_HEIGHT_OFFSET, 1080);
    advc_put_u32(output + ADVC_OUTPUT_ANDROID_FORMAT_OFFSET, 21);
    advc_put_u32(output + ADVC_OUTPUT_STRIDE_OFFSET, 1920);
    advc_put_u32(output + ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET, ADVC_FD_NONE);
    advc_put_u32(output + ADVC_OUTPUT_SLICE_HEIGHT_OFFSET, 1088);
    advc_put_u32(output + ADVC_OUTPUT_CROP_RIGHT_OFFSET, 1919);
    advc_put_u32(output + ADVC_OUTPUT_CROP_BOTTOM_OFFSET, 1079);
    {
        struct advc_message reply;
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.payload_size = sizeof(output);
        reply.header.fd_count = 1;
        reply.payload = output;
        reply.fds[0] = output_fd;
        assert(advc_send_message(fd, &reply) == 0);
    }
    close(output_fd);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_RELEASE_OUTPUT);
    assert(advc_get_u64(payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET) == 77);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_FLUSH && request.header.payload_size == 0);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CLOSE_SESSION && request.header.payload_size == 0);
    send_status(fd, &request, 11, ADVC_STATUS_OK, -1);
    close(fd);
    _exit(0);
}

static void wait_success(pid_t child) {
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_current_client_broker_negotiation(void) {
    struct advc_broker_provider provider = {
        .feature_bits = ADVC_FEATURE_MEMFD | ADVC_FEATURE_BROKER_EGL_SURFACE,
    };
    uint64_t features = 0;
    uint32_t max_payload = 0;
    int sockets[2];
    pid_t child;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        assert(advc_broker_handle_once(sockets[1], &provider) == 0);
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    assert(advc_client_hello(sockets[0], UINT64_MAX, &features, &max_payload) == 0);
    assert((features & ADVC_FEATURE_BROKER_EGL_SURFACE) != 0);
    assert(max_payload == ADVC_MAX_PAYLOAD);
    close(sockets[0]);
    wait_success(child);
}

static int send_fake_ahb_record(int socket_fd, void *native_buffer,
                                void *userdata) {
    const char record = 'A';
    (void)userdata;
    assert(native_buffer == (void *)(uintptr_t)0x1234);
    return send(socket_fd, &record, 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static void run_ahb_submit_server(int fd) {
    uint8_t payload[ADVC_MAX_PAYLOAD];
    struct advc_message request;
    char record = 0;
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CREATE_SESSION);
    assert(request.header.session_id == 0);
    assert(advc_get_u32(payload + ADVC_CREATE_DIRECTION_OFFSET) ==
           ADVC_DIRECTION_ENCODE);
    assert(advc_get_u32(payload + ADVC_CREATE_COLOR_FORMAT_OFFSET) == 0);
    assert(advc_get_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET) ==
           ADVC_TRANSPORT_ANDROID_AHB_SURFACE);
    send_status(fd, &request, 88, ADVC_STATUS_OK, -1);
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_AHB);
    assert(request.header.payload_size == ADVC_QUEUE_AHB_SIZE);
    assert(request.header.fd_count == 1);
    assert(advc_get_u32(payload + ADVC_QUEUE_AHB_WIDTH_OFFSET) == 128);
    assert(advc_get_u32(payload + ADVC_QUEUE_AHB_HEIGHT_OFFSET) == 64);
    {
        uint8_t status[ADVC_STATUS_SIZE] = {0};
        struct advc_message reply;
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.flags = ADVC_FLAG_AHB_FOLLOWS;
        reply.header.payload_size = sizeof(status);
        reply.header.fd_count = 0;
        reply.payload = status;
        assert(advc_send_message(fd, &reply) == 0);
    }
    advc_close_message_fds(&request);
    assert(recv(fd, &record, 1, 0) == 1 && record == 'A');
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_COMPLETE_AHB);
    {
        uint8_t complete[ADVC_COMPLETE_AHB_SIZE] = {0};
        struct advc_message reply;
        int fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fence >= 0);
        advc_put_u32(complete + ADVC_COMPLETE_AHB_STATUS_OFFSET, ADVC_STATUS_OK);
        advc_put_u32(complete + ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET,
                     ADVC_FD_RELEASE_FENCE);
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.payload_size = sizeof(complete);
        reply.header.fd_count = 1;
        reply.payload = complete;
        reply.fds[0] = fence;
        assert(advc_send_message(fd, &reply) == 0);
        close(fence);
    }
    close(fd);
    _exit(0);
}

static void test_ahb_submit_client_handshake(void) {
    struct advc_client_session_config config;
    struct advc_client_ahb_input input;
    int sockets[2];
    int acquire;
    int release = -1;
    pid_t child;
    uint32_t session_id = 0;
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 128;
    config.height = 64;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.transport = ADVC_TRANSPORT_ANDROID_AHB_SURFACE;
    memset(&input, 0, sizeof(input));
    input.native_buffer = (void *)(uintptr_t)0x1234;
    input.pts_ns = 123456;
    input.width = 128;
    input.height = 64;
    input.format = 1;
    input.layers = 1;
    input.usage = UINT64_C(0x100);
    acquire = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(acquire >= 0);
    input.acquire_fence_fd = acquire;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_ahb_submit_server(sockets[1]);
    }
    close(sockets[1]);
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) ==
           ADVC_STATUS_OK);
    assert(session_id == 88);
    assert(advc_client_submit_ahb(sockets[0], session_id, &input, send_fake_ahb_record,
                                  NULL, &release, NULL) == ADVC_STATUS_OK);
    assert(release >= 0);
    close(release);
    close(acquire);
    close(sockets[0]);
    wait_success(child);
}

static void run_hostile_ahb_complete_server(int fd) {
    uint8_t payload[ADVC_MAX_PAYLOAD];
    uint8_t status_payload[ADVC_STATUS_SIZE] = {0};
    uint8_t complete[ADVC_COMPLETE_AHB_SIZE] = {0};
    struct advc_message request;
    struct advc_message reply;
    char record;
    int fence;
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_AHB);
    memset(&reply, 0, sizeof(reply));
    reply.header = request.header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.flags = ADVC_FLAG_AHB_FOLLOWS;
    reply.header.payload_size = sizeof(status_payload);
    reply.payload = status_payload;
    assert(advc_send_message(fd, &reply) == 0);
    assert(recv(fd, &record, 1, 0) == 1 && record == 'A');
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_COMPLETE_AHB);
    advc_put_u32(complete + ADVC_COMPLETE_AHB_STATUS_OFFSET,
                 ADVC_STATUS_BAD_MESSAGE);
    advc_put_u32(complete + ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET,
                 ADVC_FD_RELEASE_FENCE);
    fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fence >= 0);
    memset(&reply, 0, sizeof(reply));
    reply.header = request.header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.payload_size = sizeof(complete);
    reply.header.fd_count = 1;
    reply.payload = complete;
    reply.fds[0] = fence;
    assert(advc_send_message(fd, &reply) == 0);
    close(fence);
    assert(recv(fd, &record, 1, 0) == 0);
    close(fd);
    _exit(0);
}

static void test_hostile_ahb_complete_fd_cleanup(void) {
    struct advc_client_ahb_input input;
    int sockets[2];
    int release = -1;
    size_t before;
    pid_t child;
    memset(&input, 0, sizeof(input));
    input.native_buffer = (void *)(uintptr_t)0x1234;
    input.width = 128;
    input.height = 64;
    input.format = 1;
    input.layers = 1;
    input.usage = UINT64_C(0x100);
    input.acquire_fence_fd = -1;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_hostile_ahb_complete_server(sockets[1]);
    }
    close(sockets[1]);
    before = fd_count();
    errno = 0;
    assert(advc_client_submit_ahb(sockets[0], 88, &input, send_fake_ahb_record,
                                  NULL, &release, NULL) == -1);
    assert(errno == EPROTO && release == -1 && fd_count() == before);
    close(sockets[0]);
    wait_success(child);
}

static void send_byte_output(int fd, const struct advc_message *request,
                             uint64_t buffer_id, const uint8_t *data, size_t size,
                             uint64_t pts_ns, uint32_t flags) {
    uint8_t output[ADVC_OUTPUT_BYTES_SIZE] = {0};
    struct advc_message reply;
    int output_fd = sealed_memfd(data, size);
    advc_put_u64(output + ADVC_OUTPUT_BUFFER_ID_OFFSET, buffer_id);
    advc_put_u64(output + ADVC_OUTPUT_PTS_NS_OFFSET, pts_ns);
    advc_put_u64(output + ADVC_OUTPUT_SIZE_OFFSET, size);
    advc_put_u32(output + ADVC_OUTPUT_FLAGS_OFFSET, flags);
    advc_put_u32(output + ADVC_OUTPUT_TRANSPORT_OFFSET, ADVC_TRANSPORT_BYTES);
    advc_put_u32(output + ADVC_OUTPUT_WIDTH_OFFSET, 16);
    advc_put_u32(output + ADVC_OUTPUT_HEIGHT_OFFSET, 16);
    memset(&reply, 0, sizeof(reply));
    reply.header = request->header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.payload_size = sizeof(output);
    reply.header.fd_count = 1;
    reply.payload = output;
    reply.fds[0] = output_fd;
    assert(advc_send_message(fd, &reply) == 0);
    close(output_fd);
}

static void run_encode_server(int fd) {
    static uint8_t payload[ADVC_MAX_PAYLOAD];
    static const uint8_t config_bytes[] = {0, 0, 0, 1, 0x67};
    static const uint8_t frame_bytes[] = {0, 0, 0, 1, 0x65, 0xaa};
    struct advc_message request;

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CREATE_SESSION);
    assert(advc_get_u32(payload + ADVC_CREATE_DIRECTION_OFFSET) == ADVC_DIRECTION_ENCODE);
    assert(advc_get_u32(payload + ADVC_CREATE_WIDTH_OFFSET) == 16);
    assert(advc_get_u32(payload + ADVC_CREATE_HEIGHT_OFFSET) == 16);
    assert(advc_get_u32(payload + ADVC_CREATE_BITRATE_OFFSET) == 1000000);
    assert(advc_get_u32(payload + ADVC_CREATE_FRAMERATE_MILLI_OFFSET) == 30000);
    assert(advc_get_u32(payload + ADVC_CREATE_COLOR_FORMAT_OFFSET) ==
           ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR);
    assert(strcmp((char *)payload + ADVC_CREATE_MIME_OFFSET, "video/avc") == 0);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_INPUT && request.header.session_id == 22);
    assert(request.header.fd_count == 0);
    assert(request.header.payload_size == ADVC_QUEUE_INPUT_SIZE + 384);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) == 384);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) == 0);
    for (size_t i = 0; i < 384; ++i)
        assert(payload[ADVC_QUEUE_INPUT_SIZE + i] == (uint8_t)i);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_INPUT);
    assert(request.header.payload_size == ADVC_QUEUE_INPUT_SIZE);
    assert(advc_get_u64(payload + ADVC_QUEUE_INPUT_SIZE_OFFSET) == 0);
    assert(advc_get_u32(payload + ADVC_QUEUE_INPUT_FLAGS_OFFSET) == ADVC_FLAG_END_OF_STREAM);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_DEQUEUE_OUTPUT);
    send_byte_output(fd, &request, 91, config_bytes, sizeof(config_bytes), 0,
                     ADVC_FLAG_CODEC_CONFIG);
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_RELEASE_OUTPUT);
    assert(advc_get_u64(payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET) == 91);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_DEQUEUE_OUTPUT);
    send_byte_output(fd, &request, 92, frame_bytes, sizeof(frame_bytes), 33333000,
                     ADVC_FLAG_KEY_FRAME | ADVC_FLAG_END_OF_STREAM);
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_RELEASE_OUTPUT);
    assert(advc_get_u64(payload + ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET) == 92);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_FLUSH);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CLOSE_SESSION);
    send_status(fd, &request, 22, ADVC_STATUS_OK, -1);
    close(fd);
    _exit(0);
}

static void test_happy_path(void) {
    struct advc_client_session_config config;
    struct advc_client_input input;
    struct advc_client_output output;
    struct advc_avc_annexb_parts parts;
    uint8_t readback[6];
    uint32_t session_id = 0;
    int sockets[2];
    int input_fd;
    pid_t child;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_happy_server(sockets[1]);
    }
    close(sockets[1]);
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 1920;
    config.height = 1080;
    config.framerate_milli = 60000;
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) ==
           ADVC_STATUS_OK);
    assert(session_id == 11);

    assert(advc_avc_annexb_split(avc_sample, sizeof(avc_sample), &parts) == 0);
    assert(parts.config_offset == 0 && parts.config_size == AVC_CONFIG_SIZE);
    assert(parts.frame_offset == AVC_FRAME_OFFSET && parts.frame_size == AVC_FRAME_SIZE);
    input_fd = sealed_memfd(avc_sample, sizeof(avc_sample));
    memset(&input, 0, sizeof(input));
    input.data_fd = input_fd;
    input.data_offset = parts.config_offset;
    input.size = parts.config_size;
    input.buffer_id = 1;
    input.flags = ADVC_FLAG_CODEC_CONFIG;
    assert(advc_client_queue_input(sockets[0], session_id, &input, NULL) == ADVC_STATUS_OK);
    assert(fcntl(input_fd, F_GETFD) >= 0); /* Caller still owns the input fd. */

    input.data_offset = parts.frame_offset;
    input.size = parts.frame_size;
    input.buffer_id = 2;
    input.flags = ADVC_FLAG_KEY_FRAME;
    assert(advc_client_queue_input(sockets[0], session_id, &input, NULL) == ADVC_STATUS_OK);
    assert(fcntl(input_fd, F_GETFD) >= 0);

    memset(&input, 0, sizeof(input));
    input.data_fd = -1;
    input.buffer_id = 3;
    input.flags = ADVC_FLAG_END_OF_STREAM;
    assert(advc_client_queue_input(sockets[0], session_id, &input, NULL) == ADVC_STATUS_OK);

    assert(advc_client_dequeue_output(sockets[0], session_id, &output, NULL) ==
           ADVC_STATUS_WOULD_BLOCK);
    assert(output.data_fd == -1);
    assert(advc_client_dequeue_output(sockets[0], session_id, &output, NULL) ==
           ADVC_STATUS_OK);
    assert(output.data_fd >= 0 && output.buffer_id == 77 && output.size == sizeof(readback));
    assert(output.width == 1920 && output.height == 1080 && output.slice_height == 1088);
    assert(pread(output.data_fd, readback, sizeof(readback), 0) == (ssize_t)sizeof(readback));
    assert(memcmp(readback, (uint8_t[]){1, 2, 3, 4, 5, 6}, sizeof(readback)) == 0);
    advc_client_output_close(&output);
    assert(output.data_fd == -1);
    assert(advc_client_release_output(sockets[0], session_id, output.buffer_id, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_flush(sockets[0], session_id, NULL) == ADVC_STATUS_OK);
    assert(advc_client_close_session(sockets[0], session_id, NULL) == ADVC_STATUS_OK);
    close(input_fd);
    close(sockets[0]);
    wait_success(child);
}

static void test_encode_client_path(void) {
    struct advc_client_session_config config;
    struct advc_client_input input;
    struct advc_client_output output;
    uint8_t frame[384];
    uint8_t readback[6];
    uint32_t session_id = 0;
    size_t frame_size = 0;
    int sockets[2];
    pid_t child;
    for (size_t i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)i;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_encode_server(sockets[1]);
    }
    close(sockets[1]);
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 16;
    config.height = 16;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.color_format = ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR;
    assert(advc_client_encode_frame_size(&config, &frame_size) == 0);
    assert(frame_size == sizeof(frame));
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) ==
           ADVC_STATUS_OK);
    assert(session_id == 22);

    memset(&input, 0, sizeof(input));
    input.data = frame;
    input.data_fd = -1;
    input.size = sizeof(frame);
    input.pts_ns = 33333000;
    assert(advc_client_queue_input(sockets[0], session_id, &input, NULL) ==
           ADVC_STATUS_OK);
    input.data = NULL;
    input.size = 0;
    input.flags = ADVC_FLAG_END_OF_STREAM;
    assert(advc_client_queue_input(sockets[0], session_id, &input, NULL) ==
           ADVC_STATUS_OK);

    assert(advc_client_dequeue_output(sockets[0], session_id, &output, NULL) ==
           ADVC_STATUS_OK);
    assert(output.flags == ADVC_FLAG_CODEC_CONFIG && output.size == 5);
    advc_client_output_close(&output);
    assert(advc_client_release_output(sockets[0], session_id, output.buffer_id, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_dequeue_output(sockets[0], session_id, &output, NULL) ==
           ADVC_STATUS_OK);
    assert(output.flags == (ADVC_FLAG_KEY_FRAME | ADVC_FLAG_END_OF_STREAM));
    assert(output.pts_ns == 33333000 && output.size == sizeof(readback));
    assert(pread(output.data_fd, readback, sizeof(readback), 0) == (ssize_t)sizeof(readback));
    assert(memcmp(readback, (uint8_t[]){0, 0, 0, 1, 0x65, 0xaa},
                  sizeof(readback)) == 0);
    advc_client_output_close(&output);
    assert(advc_client_release_output(sockets[0], session_id, output.buffer_id, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_flush(sockets[0], session_id, NULL) == ADVC_STATUS_OK);
    assert(advc_client_close_session(sockets[0], session_id, NULL) == ADVC_STATUS_OK);
    close(sockets[0]);
    wait_success(child);
}

static size_t fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++count;
    }
    closedir(directory);
    return count;
}

static void run_bad_status_server(int fd, int attach_fd) {
    static uint8_t payload[ADVC_MAX_PAYLOAD];
    struct advc_message request;
    int extra_fd = -1;
    receive_request(fd, &request, payload);
    if (attach_fd) extra_fd = sealed_memfd(NULL, 0);
    send_status(fd, &request, 0, attach_fd ? ADVC_STATUS_OK : 99, extra_fd);
    if (extra_fd >= 0) close(extra_fd);
    close(fd);
    _exit(0);
}

static void test_bad_status_and_fd_cleanup(int attach_fd) {
    struct advc_client_session_config config;
    uint32_t session_id = 0;
    size_t before;
    int sockets[2];
    pid_t child;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_bad_status_server(sockets[1], attach_fd);
    }
    close(sockets[1]);
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 1280;
    config.height = 720;
    before = fd_count();
    errno = 0;
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) == -1);
    assert(errno == EPROTO);
    assert(fd_count() == before);
    close(sockets[0]);
    wait_success(child);
}

static void test_local_rejections(void) {
    struct advc_client_session_config config;
    struct advc_client_ahb_input ahb_input;
    struct advc_client_input input;
    struct advc_client_output output;
    memset(&input, 0, sizeof(input));
    input.data_fd = -1;
    input.size = ADVC_MAX_INPUT_BYTES + 1u;
    errno = 0;
    assert(advc_client_queue_input(-1, 1, &input, NULL) == -1 && errno == EINVAL);
    errno = 0;
    assert(advc_client_dequeue_output(-1, 0, &output, NULL) == -1 && errno == EINVAL);
    errno = 0;
    assert(advc_client_release_output(-1, 1, 0, NULL) == -1 && errno == EINVAL);
    {
        struct advc_dmabuf_descriptor reserved;
        errno = 0;
        assert(advc_client_reserve_linear(-1, 1, 7, 1279, 720, &reserved,
                                          NULL) == -1 && errno == EINVAL);
    }
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 17;
    config.height = 16;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.color_format = ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR;
    errno = 0;
    assert(advc_client_encode_frame_size(&config, &input.size) == -1 && errno == EINVAL);
    config.width = 128;
    config.height = 64;
    config.transport = ADVC_TRANSPORT_BROKER_EGL_SURFACE;
    errno = 0;
    assert(advc_client_create_session(-1, &config, &output.flags, NULL) == -1 &&
           errno == EINVAL);
    config.color_format = 0;
    errno = 0;
    assert(advc_client_create_session(-1, &config, &output.flags, NULL) == -1 &&
           errno == EBADF);
    memset(&ahb_input, 0, sizeof(ahb_input));
    ahb_input.native_buffer = (void *)(uintptr_t)1;
    ahb_input.width = 128;
    ahb_input.height = 64;
    ahb_input.format = 1;
    ahb_input.layers = 0;
    ahb_input.acquire_fence_fd = -1;
    errno = 0;
    assert(advc_client_submit_ahb(-1, 1, &ahb_input, send_fake_ahb_record,
                                  NULL, &input.data_fd, NULL) == -1 &&
           errno == EINVAL);
    ahb_input.layers = 1;
    ahb_input.acquire_fence_fd = -2;
    errno = 0;
    assert(advc_client_submit_ahb(-1, 1, &ahb_input, send_fake_ahb_record,
                                  NULL, &input.data_fd, NULL) == -1 &&
           errno == EINVAL);
}

static void run_ahb_server(int fd) {
    static uint8_t payload[ADVC_MAX_PAYLOAD];
    uint8_t output[ADVC_OUTPUT_READY_SIZE] = {0};
    uint8_t status_payload[ADVC_STATUS_SIZE] = {0};
    struct advc_message request;
    struct advc_message reply;
    const uint8_t native_marker = 0x5a;
    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CREATE_SESSION);
    assert(advc_get_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET) ==
           ADVC_TRANSPORT_AHARDWAREBUFFER);
    send_status(fd, &request, 33, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_RESERVE_LINEAR);
    assert(request.header.payload_size == ADVC_RESERVE_LINEAR_SIZE &&
           request.header.fd_count == 0 &&
           advc_get_u64(payload + ADVC_RESERVE_LINEAR_PTS_NS_OFFSET) == 9000 &&
           advc_get_u32(payload + ADVC_RESERVE_LINEAR_WIDTH_OFFSET) == 1280 &&
           advc_get_u32(payload + ADVC_RESERVE_LINEAR_HEIGHT_OFFSET) == 720 &&
           advc_get_u32(payload + ADVC_RESERVE_LINEAR_FOURCC_OFFSET) ==
               UINT32_C(0x3231564e));
    {
        uint8_t prime[ADVC_RESERVE_LINEAR_REPLY_SIZE] = {0};
        struct advc_dmabuf_descriptor descriptor;
        int encoded_fds[ADVC_MAX_DMABUF_OBJECTS];
        uint16_t encoded_fd_count = 0;
        int object_fd = (int)syscall(SYS_memfd_create, "advc-client-reserve",
                                     MFD_CLOEXEC | MFD_ALLOW_SEALING);
        assert(object_fd >= 0 && ftruncate(object_fd, 2 * 1024 * 1024) == 0);
        memset(&descriptor, 0, sizeof(descriptor));
        for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
            descriptor.objects[i].fd = -1;
        descriptor.buffer_id = UINT64_C(0x8000000000000001);
        descriptor.width = 1280;
        descriptor.height = 720;
        descriptor.drm_fourcc = UINT32_C(0x3231564e);
        descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
        descriptor.crop_width = 1280;
        descriptor.crop_height = 720;
        descriptor.object_count = 1;
        descriptor.plane_count = 2;
        descriptor.objects[0].fd = object_fd;
        descriptor.objects[0].size = 2 * 1024 * 1024;
        descriptor.planes[0].pitch = 1280;
        descriptor.planes[1].offset = 1280 * 720;
        descriptor.planes[1].pitch = 1280;
        assert(advc_dmabuf_registration_encode(
                   prime + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
                   &descriptor, encoded_fds, &encoded_fd_count) == 0);
        advc_put_u32(prime + ADVC_STATUS_CODE_OFFSET, ADVC_STATUS_OK);
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.payload_size = sizeof(prime);
        reply.header.fd_count = encoded_fd_count;
        reply.payload = prime;
        reply.fds[0] = object_fd;
        assert(advc_send_message(fd, &reply) == 0);
        close(object_fd);
    }

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_DEQUEUE_OUTPUT);
    advc_put_u64(output + ADVC_OUTPUT_BUFFER_ID_OFFSET, 101);
    advc_put_u64(output + ADVC_OUTPUT_PTS_NS_OFFSET, 9000);
    advc_put_u32(output + ADVC_OUTPUT_TRANSPORT_OFFSET,
                 ADVC_TRANSPORT_AHARDWAREBUFFER);
    advc_put_u32(output + ADVC_OUTPUT_WIDTH_OFFSET, 1280);
    advc_put_u32(output + ADVC_OUTPUT_HEIGHT_OFFSET, 720);
    advc_put_u32(output + ADVC_OUTPUT_ANDROID_FORMAT_OFFSET, 0x22);
    advc_put_u32(output + ADVC_OUTPUT_STRIDE_OFFSET, 1280);
    advc_put_u32(output + ADVC_OUTPUT_LAYERS_OFFSET, 1);
    advc_put_u64(output + ADVC_OUTPUT_USAGE_OFFSET, UINT64_C(0x300));
    memset(&reply, 0, sizeof(reply));
    reply.header = request.header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.payload_size = sizeof(output);
    reply.payload = output;
    assert(advc_send_message(fd, &reply) == 0);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_TRANSFER_PRIME);
    assert(request.header.payload_size == ADVC_TRANSFER_PRIME_SIZE &&
           request.header.fd_count == 0 && advc_get_u64(payload) == 101);
    {
        uint8_t prime[ADVC_TRANSFER_PRIME_REPLY_SIZE] = {0};
        struct advc_dmabuf_descriptor descriptor;
        int encoded_fds[ADVC_MAX_DMABUF_OBJECTS];
        uint16_t encoded_fd_count = 0;
        int object_fd = (int)syscall(SYS_memfd_create, "advc-client-prime",
                                     MFD_CLOEXEC | MFD_ALLOW_SEALING);
        assert(object_fd >= 0 && ftruncate(object_fd, 2 * 1024 * 1024) == 0);
        memset(&descriptor, 0, sizeof(descriptor));
        for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
            descriptor.objects[i].fd = -1;
        descriptor.buffer_id = 101;
        descriptor.width = 1280;
        descriptor.height = 720;
        descriptor.drm_fourcc = UINT32_C(0x3231564e);
        descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
        descriptor.crop_width = 1280;
        descriptor.crop_height = 720;
        descriptor.object_count = 1;
        descriptor.plane_count = 2;
        descriptor.objects[0].fd = object_fd;
        descriptor.objects[0].size = 2 * 1024 * 1024;
        descriptor.planes[0].pitch = 1280;
        descriptor.planes[1].offset = 1280 * 720;
        descriptor.planes[1].pitch = 1280;
        assert(advc_dmabuf_registration_encode(
                   prime + ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET,
                   &descriptor, encoded_fds, &encoded_fd_count) == 0);
        advc_put_u32(prime + ADVC_STATUS_CODE_OFFSET, ADVC_STATUS_OK);
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.payload_size = sizeof(prime);
        reply.header.fd_count = encoded_fd_count;
        reply.payload = prime;
        reply.fds[0] = object_fd;
        assert(advc_send_message(fd, &reply) == 0);
        close(object_fd);
    }

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_TRANSFER_AHB);
    assert(advc_get_u64(payload) == 101);
    memset(&reply, 0, sizeof(reply));
    reply.header = request.header;
    reply.header.message_type = ADVC_MSG_REPLY;
    reply.header.flags = ADVC_FLAG_AHB_FOLLOWS;
    reply.header.payload_size = sizeof(status_payload);
    reply.payload = status_payload;
    assert(advc_send_message(fd, &reply) == 0);
    assert(send(fd, &native_marker, 1, MSG_NOSIGNAL) == 1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_RELEASE_OUTPUT);
    assert(advc_get_u64(payload) == 101);
    send_status(fd, &request, 33, ADVC_STATUS_OK, -1);
    close(fd);
    _exit(0);
}

static int receive_fake_ahb(int fd, void **native_buffer, void *userdata) {
    uint8_t marker = 0;
    (void)userdata;
    if (recv(fd, &marker, 1, 0) != 1 || marker != 0x5a) return -1;
    *native_buffer = (void *)(uintptr_t)0x1234;
    return 0;
}

static void test_ahb_client_path(void) {
    struct advc_client_session_config config;
    struct advc_client_output output;
    struct advc_dmabuf_descriptor prime;
    struct advc_dmabuf_descriptor reserved;
    void *native_buffer = NULL;
    uint32_t session_id = 0;
    int prime_status;
    int sockets[2];
    pid_t child;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_ahb_server(sockets[1]);
    }
    close(sockets[1]);
    memset(&config, 0, sizeof(config));
    config.mime = "video/avc";
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 1280;
    config.height = 720;
    config.transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_reserve_linear(sockets[0], session_id, 9000, 1280, 720,
                                      &reserved, NULL) == ADVC_STATUS_OK);
    assert(reserved.buffer_id == UINT64_C(0x8000000000000001) &&
           reserved.width == 1280 && reserved.height == 720 &&
           reserved.drm_fourcc == UINT32_C(0x3231564e) &&
           reserved.drm_modifier == 0 && reserved.object_count == 1 &&
           reserved.plane_count == 2 && reserved.objects[0].fd >= 0);
    advc_dmabuf_descriptor_close(&reserved);
    assert(advc_client_dequeue_output(sockets[0], session_id, &output, NULL) ==
           ADVC_STATUS_OK);
    assert(output.transport == ADVC_TRANSPORT_AHARDWAREBUFFER);
    assert(output.data_fd == -1 && output.acquire_fence_fd == -1);
    assert(output.layers == 1 && output.usage == UINT64_C(0x300));
    assert(output.slice_height == 720);
    assert(output.crop_left == 0 && output.crop_top == 0);
    assert(output.crop_right == 1279 && output.crop_bottom == 719);
    prime_status = advc_client_transfer_prime(
        sockets[0], session_id, output.buffer_id, &prime, NULL);
    if (prime_status != ADVC_STATUS_OK)
        fprintf(stderr, "transfer-prime status=%d errno=%d\n", prime_status,
                errno);
    assert(prime_status == ADVC_STATUS_OK);
    assert(prime.buffer_id == output.buffer_id && prime.width == 1280 &&
           prime.height == 720 && prime.drm_fourcc == UINT32_C(0x3231564e) &&
           prime.drm_modifier == 0 && prime.object_count == 1 &&
           prime.plane_count == 2 && prime.objects[0].fd >= 0);
    advc_dmabuf_descriptor_close(&prime);
    assert(advc_client_transfer_ahb(sockets[0], session_id, output.buffer_id,
                                    receive_fake_ahb, NULL, &native_buffer, NULL) ==
           ADVC_STATUS_OK);
    assert(native_buffer == (void *)(uintptr_t)0x1234);
    assert(advc_client_release_output(sockets[0], session_id, output.buffer_id, NULL) ==
           ADVC_STATUS_OK);
    advc_client_output_close(&output);
    close(sockets[0]);
    wait_success(child);
}

static void test_annexb_rejections(void) {
    static const uint8_t no_vcl[] = {
        0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x68, 0x02,
    };
    static const uint8_t no_pps[] = {
        0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x65, 0x02,
    };
    static const uint8_t non_idr[] = {
        0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x68, 0x02,
        0x00, 0x00, 0x01, 0x41, 0x03,
    };
    static const uint8_t malformed[] = {0x00, 0x00, 0x01};
    struct advc_avc_annexb_parts parts;
    errno = 0;
    assert(advc_avc_annexb_split(no_vcl, sizeof(no_vcl), &parts) == -1 && errno == EINVAL);
    errno = 0;
    assert(advc_avc_annexb_split(no_pps, sizeof(no_pps), &parts) == -1 && errno == EINVAL);
    errno = 0;
    assert(advc_avc_annexb_split(non_idr, sizeof(non_idr), &parts) == -1 && errno == EINVAL);
    errno = 0;
    assert(advc_avc_annexb_split(malformed, sizeof(malformed), &parts) == -1 && errno == EINVAL);
}

static void test_annexb_sei_before_idr(void) {
    static const uint8_t sample[] = {
        0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x68, 0x02,
        0x00, 0x00, 0x01, 0x06, 0x03,
        0x00, 0x00, 0x01, 0x65, 0x04,
    };
    struct advc_avc_annexb_parts parts;
    assert(advc_avc_annexb_split(sample, sizeof(sample), &parts) == 0);
    assert(parts.config_offset == 0 && parts.config_size == 10);
    assert(parts.frame_offset == 10 && parts.frame_size == sizeof(sample) - 10);
}

static void test_annexb_single_slice_stream(void) {
    static const uint8_t sample[] = {
        0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x01, 0x68, 0x02,
        0x00, 0x00, 0x01, 0x06, 0x03,
        0x00, 0x00, 0x01, 0x65, 0x04,
        0x00, 0x00, 0x01, 0x41, 0x05,
        0x00, 0x00, 0x01, 0x41, 0x06,
    };
    struct advc_avc_annexb_stream stream;
    assert(advc_avc_annexb_split_single_slice_stream(sample, sizeof(sample),
                                                       &stream) == 0);
    assert(stream.config_offset == 0 && stream.config_size == 10);
    assert(stream.frame_count == 3);
    assert(stream.frames[0].offset == 10 && stream.frames[0].size == 10 &&
           stream.frames[0].key_frame);
    assert(stream.frames[1].offset == 20 && stream.frames[1].size == 5 &&
           !stream.frames[1].key_frame);
    assert(stream.frames[2].offset == 25 && stream.frames[2].size == 5 &&
           !stream.frames[2].key_frame);
}

static void test_annexb_120_frame_bound(void) {
    enum { frame_count = 120, nal_size = 5, config_size = 10 };
    const size_t stream_size = config_size + frame_count * nal_size;
    uint8_t *sample = calloc(1, stream_size);
    struct advc_avc_annexb_stream stream;

    assert(sample != NULL);
    sample[2] = 1;
    sample[3] = 0x67;
    sample[4] = 1;
    sample[7] = 1;
    sample[8] = 0x68;
    sample[9] = 1;
    for (size_t i = 0; i < frame_count; ++i) {
        const size_t offset = config_size + i * nal_size;
        sample[offset + 2] = 1;
        sample[offset + 3] = i == 0 ? 0x65 : 0x41;
        sample[offset + 4] = (uint8_t)(i + 1);
    }

    assert(advc_avc_annexb_split_single_slice_stream(sample, stream_size,
                                                       &stream) == 0);
    assert(stream.config_size == config_size);
    assert(stream.frame_count == frame_count);
    assert(stream.frames[0].key_frame);
    assert(!stream.frames[frame_count - 1].key_frame);
    assert(stream.frames[frame_count - 1].offset ==
           config_size + (frame_count - 1) * nal_size);
    assert(stream.frames[frame_count - 1].size == nal_size);
    free(sample);
}

static void run_dmabuf_client_server(int fd) {
    uint8_t payload[ADVC_MAX_PAYLOAD];
    struct advc_message request;

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CREATE_SESSION);
    assert(advc_get_u32(payload + ADVC_CREATE_TRANSPORT_OFFSET) ==
           ADVC_TRANSPORT_DMABUF);
    send_status(fd, &request, 91, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_REGISTER_DMABUF);
    assert(request.header.payload_size == ADVC_REGISTER_DMABUF_SIZE);
    assert(request.header.fd_count == 1);
    assert(advc_get_u64(payload + ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET) == 44);
    assert(advc_get_u32(payload + ADVC_REGISTER_DMABUF_FOURCC_OFFSET) ==
           UINT32_C(0x34324241));
    assert(advc_get_u64(payload + ADVC_REGISTER_DMABUF_MODIFIER_OFFSET) == 0);
    assert(advc_get_u32(payload + ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET) == 1);
    assert(advc_get_u32(payload + ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET) == 1);
    assert(advc_get_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                        ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET) == 0);
    assert(advc_get_u64(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                        ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET) == 0);
    assert(advc_get_u32(payload + ADVC_REGISTER_DMABUF_PLANES_OFFSET +
                        ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET) == 512);
    advc_close_message_fds(&request);
    send_status(fd, &request, 91, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_QUEUE_DMABUF);
    assert(request.header.payload_size == ADVC_QUEUE_DMABUF_SIZE);
    assert(request.header.fd_count == 0);
    assert(advc_get_u64(payload + ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET) == 44);
    assert(advc_get_u64(payload + ADVC_QUEUE_DMABUF_PTS_NS_OFFSET) == 9000);
    assert(advc_get_u32(payload + ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET) ==
           ADVC_FD_NONE);
    send_status(fd, &request, 91, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_COMPLETE_DMABUF);
    assert(request.header.payload_size == ADVC_COMPLETE_DMABUF_REQUEST_SIZE);
    assert(advc_get_u64(payload + ADVC_COMPLETE_DMABUF_REQUEST_BUFFER_ID_OFFSET) ==
           44);
    {
        uint8_t complete[ADVC_COMPLETE_DMABUF_SIZE] = {0};
        struct advc_message reply;
        advc_put_u64(complete + ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET, 44);
        advc_put_u32(complete + ADVC_COMPLETE_DMABUF_STATUS_OFFSET,
                     ADVC_STATUS_OK);
        advc_put_u32(complete + ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET,
                     ADVC_FD_NONE);
        memset(&reply, 0, sizeof(reply));
        reply.header = request.header;
        reply.header.message_type = ADVC_MSG_REPLY;
        reply.header.payload_size = sizeof(complete);
        reply.payload = complete;
        assert(advc_send_message(fd, &reply) == 0);
    }

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_UNREGISTER_DMABUF);
    assert(advc_get_u64(payload + ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET) == 44);
    send_status(fd, &request, 91, ADVC_STATUS_OK, -1);

    receive_request(fd, &request, payload);
    assert(request.header.opcode == ADVC_OP_CLOSE_SESSION);
    send_status(fd, &request, 91, ADVC_STATUS_OK, -1);
    close(fd);
    _exit(0);
}

static void test_dmabuf_client_path(void) {
    struct advc_client_session_config config = {
        .direction = ADVC_DIRECTION_ENCODE,
        .encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE,
        .mime = "video/avc",
        .width = 128,
        .height = 64,
        .bitrate = 1000000,
        .framerate_milli = 30000,
        .transport = ADVC_TRANSPORT_DMABUF,
    };
    struct advc_dmabuf_descriptor descriptor;
    struct advc_dmabuf_submission submission = {
        .buffer_id = 44,
        .pts_ns = 9000,
        .acquire_fence_fd = -1,
    };
    uint32_t session_id = 0;
    int release_fence = -2;
    int sockets[2];
    int object_fd;
    pid_t child;

    object_fd = (int)syscall(SYS_memfd_create, "advc-client-dmabuf",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(object_fd >= 0);
    assert(ftruncate(object_fd, 128 * 64 * 4) == 0);
    assert(fcntl(object_fd, F_ADD_SEALS,
                 F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE |
                 F_SEAL_SEAL) == 0);
    memset(&descriptor, 0, sizeof(descriptor));
    for (size_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor.objects[i].fd = -1;
    descriptor.buffer_id = 44;
    descriptor.width = 128;
    descriptor.height = 64;
    descriptor.drm_fourcc = UINT32_C(0x34324241);
    descriptor.explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor.crop_width = 128;
    descriptor.crop_height = 64;
    descriptor.object_count = 1;
    descriptor.plane_count = 1;
    descriptor.color_matrix = ADVC_COLOR_MATRIX_RGB;
    descriptor.color_range = ADVC_COLOR_RANGE_FULL;
    descriptor.objects[0].fd = object_fd;
    descriptor.objects[0].size = 128 * 64 * 4;
    descriptor.planes[0].pitch = 512;

    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        run_dmabuf_client_server(sockets[1]);
    }
    close(sockets[1]);
    assert(advc_client_create_session(sockets[0], &config, &session_id, NULL) ==
           ADVC_STATUS_OK);
    assert(session_id == 91);
    assert(advc_client_register_dmabuf(sockets[0], session_id, &descriptor,
                                       NULL) == ADVC_STATUS_OK);
    assert(advc_client_queue_dmabuf(sockets[0], session_id, &submission, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_complete_dmabuf(sockets[0], session_id, 44,
                                       &release_fence, NULL) == ADVC_STATUS_OK);
    assert(release_fence == -1);
    assert(advc_client_unregister_dmabuf(sockets[0], session_id, 44, NULL) ==
           ADVC_STATUS_OK);
    assert(advc_client_close_session(sockets[0], session_id, NULL) ==
           ADVC_STATUS_OK);
    close(object_fd);
    close(sockets[0]);
    wait_success(child);
}

static long elapsed_ms(const struct timespec *start,
                       const struct timespec *finish) {
    return (finish->tv_sec - start->tv_sec) * 1000L +
           (finish->tv_nsec - start->tv_nsec) / 1000000L;
}

static int make_connect_listener(char *path, size_t capacity,
                                 unsigned int sequence, int backlog) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(snprintf(path, capacity, "/tmp/advc-client-connect-%ld-%u.sock",
                    (long)getpid(), sequence) > 0);
    unlink(path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, backlog) == 0);
    return fd;
}

static void test_bounded_connect_restores_blocking(void) {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int listener = make_connect_listener(path, sizeof(path), 1, 1);
    int client = advc_client_connect_bounded(path, 1000);
    int accepted;
    assert(client >= 0);
    assert((fcntl(client, F_GETFL) & O_NONBLOCK) == 0);
    accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    assert(accepted >= 0);
    close(accepted);
    close(client);
    close(listener);
    unlink(path);
}

static void test_bounded_connect_full_backlog(void) {
    enum { MAX_FILLERS = 16 };
    struct sockaddr_un address;
    struct timespec start;
    struct timespec finish;
    char path[sizeof(address.sun_path)];
    int fillers[MAX_FILLERS];
    size_t baseline = fd_count();
    size_t filler_count = 0;
    int listener = make_connect_listener(path, sizeof(path), 2, 1);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    while (filler_count < MAX_FILLERS) {
        int fd = socket(AF_UNIX,
                        SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            fillers[filler_count++] = fd;
            continue;
        }
        assert(errno == EAGAIN);
        close(fd);
        break;
    }
    assert(filler_count > 0 && filler_count < MAX_FILLERS);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    errno = 0;
    assert(advc_client_connect_bounded(path, 100) < 0);
    assert(errno == ETIMEDOUT);
    assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    assert(elapsed_ms(&start, &finish) >= 50L);
    assert(elapsed_ms(&start, &finish) < 1000L);
    while (filler_count > 0) close(fillers[--filler_count]);
    close(listener);
    unlink(path);
    assert(fd_count() == baseline);
}

int main(void) {
    test_bounded_connect_restores_blocking();
    test_bounded_connect_full_backlog();
    test_ahb_submit_client_handshake();
    test_hostile_ahb_complete_fd_cleanup();
    test_current_client_broker_negotiation();
    test_happy_path();
    test_encode_client_path();
    test_bad_status_and_fd_cleanup(0);
    test_bad_status_and_fd_cleanup(1);
    test_local_rejections();
    test_ahb_client_path();
    test_dmabuf_client_path();
    test_annexb_rejections();
    test_annexb_sei_before_idr();
    test_annexb_single_slice_stream();
    test_annexb_120_frame_bound();
    return 0;
}
