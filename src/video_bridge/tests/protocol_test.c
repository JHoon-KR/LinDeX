#define _GNU_SOURCE
#include "advc/broker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct advc_message make_message(uint16_t type, uint16_t opcode, uint32_t id,
                                        uint8_t *payload, size_t capacity) {
    struct advc_message message;
    memset(&message, 0, sizeof(message));
    message.header.version_major = ADVC_VERSION_MAJOR;
    message.header.version_minor = ADVC_VERSION_MINOR;
    message.header.message_type = type;
    message.header.opcode = opcode;
    message.header.request_id = id;
    message.payload = payload;
    message.payload_capacity = capacity;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) message.fds[i] = -1;
    return message;
}

static int mock_caps(void *unused, struct advc_capability_set *caps,
                     char *error, size_t error_size) {
    (void)unused;
    (void)error;
    (void)error_size;
    memset(caps, 0, sizeof(*caps));
    caps->api_level = 35;
    caps->transport_features = ADVC_FEATURE_AHARDWAREBUFFER |
                               ADVC_FEATURE_DMABUF |
                               ADVC_FEATURE_DMABUF_EGL |
                               ADVC_FEATURE_DMABUF_VULKAN |
                               ADVC_FEATURE_NATIVE_FENCE |
                               ADVC_FEATURE_BROKER_EGL_SURFACE |
                               ADVC_FEATURE_ANDROID_AHB_SURFACE |
                               ADVC_FEATURE_DECODE_PRIME |
                               ADVC_FEATURE_ASYNC_DECODE_PRIME |
                               ADVC_FEATURE_DECODE_QCOM_MODIFIER |
                               ADVC_FEATURE_ENCODE_QCOM_MODIFIER;
    caps->count = 1;
    strcpy(caps->codecs[0].mime, "video/hevc");
    strcpy(caps->codecs[0].codec_name, "c2.qti.hevc.decoder");
    caps->codecs[0].direction = ADVC_DIRECTION_DECODE;
    caps->codecs[0].acceleration = ADVC_ACCELERATION_HARDWARE;
    caps->codecs[0].max_width = 7680;
    caps->codecs[0].max_height = 4320;
    caps->codecs[0].max_fps_milli = 120000;
    return 0;
}

static void test_header_round_trip(void) {
    uint8_t bytes[ADVC_HEADER_SIZE];
    struct advc_header input = {
        .version_major = ADVC_VERSION_MAJOR,
        .version_minor = ADVC_VERSION_MINOR,
        .message_type = ADVC_MSG_REQUEST,
        .opcode = ADVC_OP_QUEUE_INPUT,
        .request_id = 0x11223344,
        .session_id = 7,
        .flags = ADVC_FLAG_KEY_FRAME,
        .payload_size = 99,
        .fd_count = 2,
    };
    struct advc_header output;
    assert(advc_header_encode(bytes, &input) == ADVC_HEADER_SIZE);
    assert(advc_get_u32(bytes) == ADVC_MAGIC);
    assert(advc_header_decode(&output, bytes) == 0);
    assert(output.request_id == input.request_id);
    assert(output.payload_size == input.payload_size);
    assert(output.fd_count == input.fd_count);
}

static void test_fd_round_trip(void) {
    int sockets[2];
    int pipe_fds[2];
    uint8_t send_payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t receive_payload[8];
    char byte = 'x';
    struct advc_message sent = make_message(ADVC_MSG_REQUEST, ADVC_OP_QUEUE_INPUT, 2,
                                            send_payload, sizeof(send_payload));
    struct advc_message received = make_message(ADVC_MSG_REQUEST, ADVC_OP_QUEUE_INPUT, 0,
                                                receive_payload, sizeof(receive_payload));
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    sent.header.payload_size = sizeof(send_payload);
    sent.header.fd_count = 1;
    sent.fds[0] = pipe_fds[0];
    assert(advc_send_message(sockets[0], &sent) == 0);
    assert(advc_receive_message(sockets[1], &received) == 0);
    assert(received.header.fd_count == 1);
    assert(memcmp(send_payload, receive_payload, sizeof(send_payload)) == 0);
    assert((fcntl(received.fds[0], F_GETFD) & FD_CLOEXEC) != 0);
    assert(write(pipe_fds[1], &byte, 1) == 1);
    assert(read(received.fds[0], &byte, 1) == 1);
    advc_close_message_fds(&received);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(sockets[0]);
    close(sockets[1]);
}

static void send_request_minor(int fd, uint16_t minor, uint16_t opcode,
                               uint32_t id, uint32_t session_id) {
    uint8_t payload[1];
    struct advc_message request = make_message(ADVC_MSG_REQUEST, opcode, id, payload, sizeof(payload));
    request.header.version_minor = minor;
    request.header.session_id = session_id;
    assert(advc_send_message(fd, &request) == 0);
}

static void send_request(int fd, uint16_t opcode, uint32_t id) {
    send_request_minor(fd, ADVC_VERSION_MINOR, opcode, id, 0);
}

static uint32_t mock_codec_compat(void *unused,
                                  const struct advc_message *request,
                                  struct advc_message *reply) {
    (void)unused;
    assert(request->header.version_minor == 2);
    if (request->header.opcode == ADVC_OP_CREATE_SESSION)
        reply->header.session_id = 77;
    advc_put_u32(reply->payload + ADVC_STATUS_CODE_OFFSET, ADVC_STATUS_OK);
    advc_put_u32(reply->payload + ADVC_STATUS_DETAIL_OFFSET, 0);
    reply->header.payload_size = ADVC_STATUS_SIZE;
    return ADVC_STATUS_OK;
}

static struct advc_message receive_reply(int fd, uint8_t *payload, size_t size) {
    struct advc_message reply = make_message(ADVC_MSG_REPLY, ADVC_OP_PING, 0, payload, size);
    assert(advc_receive_message(fd, &reply) == 0);
    return reply;
}

static void test_broker(void) {
    int sockets[2];
    uint8_t payload[ADVC_MAX_PAYLOAD];
    struct advc_broker_provider provider = {
        .feature_bits = ADVC_FEATURE_MEMFD | ADVC_FEATURE_AHARDWAREBUFFER |
                        ADVC_FEATURE_DMABUF |
                        ADVC_FEATURE_DMABUF_EGL |
                        ADVC_FEATURE_DMABUF_VULKAN |
                        ADVC_FEATURE_NATIVE_FENCE |
                        ADVC_FEATURE_BROKER_EGL_SURFACE |
                        ADVC_FEATURE_ANDROID_AHB_SURFACE |
                        ADVC_FEATURE_DECODE_PRIME |
                        ADVC_FEATURE_ASYNC_DECODE_PRIME |
                        ADVC_FEATURE_DECODE_QCOM_MODIFIER |
                        ADVC_FEATURE_ENCODE_QCOM_MODIFIER,
        .query_capabilities = mock_caps,
        .handle_codec_request = mock_codec_compat,
    };
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);

    send_request(sockets[0], ADVC_OP_HELLO, 10);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(hello.header.request_id == 10);
    assert(hello.header.payload_size == ADVC_HELLO_SIZE);
    assert((advc_get_u64(payload) & ADVC_FEATURE_AHARDWAREBUFFER) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_BROKER_EGL_SURFACE) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ANDROID_AHB_SURFACE) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF_EGL) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF_VULKAN) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_PRIME) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ASYNC_DECODE_PRIME) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_QCOM_MODIFIER) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ENCODE_QCOM_MODIFIER) != 0);

    send_request(sockets[0], ADVC_OP_QUERY_CAPABILITIES, 11);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message caps = receive_reply(sockets[0], payload, sizeof(payload));
    assert(caps.header.request_id == 11);
    assert(advc_get_u32(payload) == ADVC_STATUS_OK);
    assert(advc_get_u32(payload + 4) == 1);
    assert(advc_get_u32(payload + 8) == 35);
    assert((advc_get_u64(payload + 16) & ADVC_FEATURE_NATIVE_FENCE) != 0);
    assert((advc_get_u64(payload + 16) & ADVC_FEATURE_DECODE_PRIME) != 0);
    assert((advc_get_u64(payload + 16) &
            ADVC_FEATURE_DECODE_QCOM_MODIFIER) != 0);

    send_request_minor(sockets[0], 7, ADVC_OP_HELLO, 23, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v17_hello =
        receive_reply(sockets[0], payload, sizeof(payload));
    assert(v17_hello.header.version_minor == 7);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_PRIME) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ASYNC_DECODE_PRIME) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_QCOM_MODIFIER) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ENCODE_QCOM_MODIFIER) == 0);
    assert(strcmp((char *)payload + ADVC_CAPS_PREFIX_SIZE, "video/hevc") == 0);
    assert(strcmp((char *)payload + ADVC_CAPS_PREFIX_SIZE + ADVC_CAPS_ENTRY_NAME_OFFSET,
                  "c2.qti.hevc.decoder") == 0);

    /* A provider bit must not leak to pre-1.6 clients. */
    send_request_minor(sockets[0], 0, ADVC_OP_HELLO, 12, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v10_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(v10_hello.header.version_minor == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_BROKER_EGL_SURFACE) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_PRIME) == 0);

    send_request_minor(sockets[0], 5, ADVC_OP_HELLO, 22, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v15_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(v15_hello.header.version_minor == 5);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_PRIME) == 0);

    send_request_minor(sockets[0], 1, ADVC_OP_HELLO, 13, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v11_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(v11_hello.header.version_minor == 1);
    assert((advc_get_u64(payload) & ADVC_FEATURE_BROKER_EGL_SURFACE) == 0);

    send_request_minor(sockets[0], 2, ADVC_OP_HELLO, 14, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message old_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(old_hello.header.version_minor == 2);
    assert((advc_get_u64(payload) & ADVC_FEATURE_BROKER_EGL_SURFACE) == 0);

    send_request_minor(sockets[0], 2, ADVC_OP_QUERY_CAPABILITIES, 15, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message old_caps = receive_reply(sockets[0], payload, sizeof(payload));
    assert(old_caps.header.version_minor == 2);
    assert((advc_get_u64(payload + 16) & ADVC_FEATURE_BROKER_EGL_SURFACE) == 0);

    send_request_minor(sockets[0], 3, ADVC_OP_HELLO, 20, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v13_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(v13_hello.header.version_minor == 3);
    assert((advc_get_u64(payload) & ADVC_FEATURE_BROKER_EGL_SURFACE) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ANDROID_AHB_SURFACE) == 0);

    send_request_minor(sockets[0], 4, ADVC_OP_HELLO, 21, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message v14_hello = receive_reply(sockets[0], payload, sizeof(payload));
    assert(v14_hello.header.version_minor == 4);
    assert((advc_get_u64(payload) & ADVC_FEATURE_ANDROID_AHB_SURFACE) != 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF_EGL) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DMABUF_VULKAN) == 0);
    assert((advc_get_u64(payload) & ADVC_FEATURE_DECODE_PRIME) == 0);

    send_request_minor(sockets[0], 2, ADVC_OP_CREATE_SESSION, 16, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message old_create = receive_reply(sockets[0], payload, sizeof(payload));
    assert(old_create.header.version_minor == 2 && old_create.header.session_id == 77);
    assert(advc_get_u32(payload) == ADVC_STATUS_OK);

    send_request_minor(sockets[0], 2, ADVC_OP_QUEUE_INPUT, 17, 77);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message old_queue = receive_reply(sockets[0], payload, sizeof(payload));
    assert(old_queue.header.version_minor == 2 && old_queue.header.session_id == 77);
    assert(advc_get_u32(payload) == ADVC_STATUS_OK);

    send_request_minor(sockets[0], 2, ADVC_OP_DEQUEUE_OUTPUT, 18, 77);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message old_dequeue = receive_reply(sockets[0], payload, sizeof(payload));
    assert(old_dequeue.header.version_minor == 2 && old_dequeue.header.session_id == 77);
    assert(advc_get_u32(payload) == ADVC_STATUS_OK);

    send_request_minor(sockets[0], ADVC_VERSION_MINOR + 1, ADVC_OP_PING, 19, 0);
    assert(advc_broker_handle_once(sockets[1], &provider) == 0);
    struct advc_message future = receive_reply(sockets[0], payload, sizeof(payload));
    assert(future.header.version_minor == ADVC_VERSION_MINOR);
    assert(advc_get_u32(payload) == ADVC_STATUS_UNSUPPORTED);
    assert(advc_get_u32(payload + ADVC_STATUS_DETAIL_OFFSET) == ADVC_VERSION_MINOR);

    close(sockets[0]);
    close(sockets[1]);
}

static void test_json_protocol_version(void) {
    struct advc_capability_set caps;
    char json[512];
    memset(&caps, 0, sizeof(caps));
    caps.api_level = 36;
    assert(advc_capabilities_write_json(&caps, json, sizeof(json)) > 0);
    assert(strstr(json, "\"protocol\":\"1.8\"") != NULL);
}

int main(void) {
    test_header_round_trip();
    test_fd_round_trip();
    test_broker();
    test_json_protocol_version();
    puts("protocol_test: all tests passed");
    return 0;
}
