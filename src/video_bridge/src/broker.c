#include "advc/broker.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void init_reply(struct advc_message *reply, const struct advc_message *request,
                       uint8_t *payload, size_t capacity) {
    memset(reply, 0, sizeof(*reply));
    reply->header.version_major = ADVC_VERSION_MAJOR;
    reply->header.version_minor = request->header.version_minor <= ADVC_VERSION_MINOR ?
                                  request->header.version_minor : ADVC_VERSION_MINOR;
    reply->header.message_type = ADVC_MSG_REPLY;
    reply->header.opcode = request->header.opcode;
    reply->header.request_id = request->header.request_id;
    reply->header.session_id = request->header.session_id;
    reply->payload = payload;
    reply->payload_capacity = capacity;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) reply->fds[i] = -1;
}

static void status_reply(struct advc_message *reply, uint32_t status, uint32_t detail) {
    advc_put_u32(reply->payload + ADVC_STATUS_CODE_OFFSET, status);
    advc_put_u32(reply->payload + ADVC_STATUS_DETAIL_OFFSET, detail);
    reply->header.payload_size = ADVC_STATUS_SIZE;
}

static int write_capability_reply(struct advc_message *reply, const struct advc_capability_set *caps) {
    size_t required = ADVC_CAPS_PREFIX_SIZE + caps->count * ADVC_CAPS_ENTRY_SIZE;
    if (caps->count > ADVC_MAX_CAPABILITIES || required > reply->payload_capacity) return -1;
    memset(reply->payload, 0, required);
    advc_put_u32(reply->payload + 0, ADVC_STATUS_OK);
    advc_put_u32(reply->payload + 4, caps->count);
    advc_put_u32(reply->payload + 8, caps->api_level);
    advc_put_u32(reply->payload + 12, 0);
    advc_put_u64(reply->payload + 16, caps->transport_features);
    for (uint32_t i = 0; i < caps->count; ++i) {
        uint8_t *entry = reply->payload + ADVC_CAPS_PREFIX_SIZE + i * ADVC_CAPS_ENTRY_SIZE;
        const struct advc_codec_capability *c = &caps->codecs[i];
        strncpy((char *)entry + ADVC_CAPS_ENTRY_MIME_OFFSET, c->mime, ADVC_MAX_MIME - 1);
        strncpy((char *)entry + ADVC_CAPS_ENTRY_NAME_OFFSET, c->codec_name, ADVC_MAX_CODEC_NAME - 1);
        entry[ADVC_CAPS_ENTRY_DIRECTION_OFFSET] = c->direction;
        entry[ADVC_CAPS_ENTRY_ACCELERATION_OFFSET] = c->acceleration;
        entry[ADVC_CAPS_ENTRY_LOW_LATENCY_OFFSET] = c->low_latency;
        entry[ADVC_CAPS_ENTRY_SECURE_OFFSET] = c->secure_playback;
        advc_put_u32(entry + ADVC_CAPS_ENTRY_MAX_WIDTH_OFFSET, c->max_width);
        advc_put_u32(entry + ADVC_CAPS_ENTRY_MAX_HEIGHT_OFFSET, c->max_height);
        advc_put_u32(entry + ADVC_CAPS_ENTRY_MAX_FPS_MILLI_OFFSET, c->max_fps_milli);
        advc_put_u32(entry + ADVC_CAPS_ENTRY_FLAGS_OFFSET, c->flags);
    }
    reply->header.payload_size = (uint32_t)required;
    return 0;
}

int advc_broker_handle_once(int client_fd, const struct advc_broker_provider *provider) {
    uint8_t *request_payload = NULL;
    uint8_t *reply_payload = NULL;
    struct advc_message request;
    struct advc_message reply;
    int result = -1;

    request_payload = (uint8_t *)malloc(ADVC_MAX_PAYLOAD);
    reply_payload = (uint8_t *)malloc(ADVC_MAX_PAYLOAD);
    if (request_payload == NULL || reply_payload == NULL) {
        errno = ENOMEM;
        goto done;
    }
    memset(&request, 0, sizeof(request));
    request.payload = request_payload;
    request.payload_capacity = ADVC_MAX_PAYLOAD;
    for (size_t i = 0; i < ADVC_MAX_FDS; ++i) request.fds[i] = -1;
    if (advc_receive_message(client_fd, &request) < 0) goto done;
    if (request.header.message_type != ADVC_MSG_REQUEST) {
        errno = EPROTO;
        goto close_request_fds;
    }

    init_reply(&reply, &request, reply_payload, ADVC_MAX_PAYLOAD);
    if (request.header.version_minor > ADVC_VERSION_MINOR) {
        status_reply(&reply, ADVC_STATUS_UNSUPPORTED, ADVC_VERSION_MINOR);
        goto send_reply;
    }
    switch (request.header.opcode) {
    case ADVC_OP_HELLO: {
        uint64_t client_features = UINT64_MAX;
        uint32_t client_max_payload = ADVC_MAX_PAYLOAD;
        if (request.header.payload_size != 0 && request.header.payload_size != ADVC_HELLO_SIZE) {
            status_reply(&reply, ADVC_STATUS_BAD_MESSAGE, 0);
            break;
        }
        if (request.header.payload_size == ADVC_HELLO_SIZE) {
            client_features = advc_get_u64(request.payload + ADVC_HELLO_FEATURES_OFFSET);
            client_max_payload = advc_get_u32(request.payload + ADVC_HELLO_MAX_PAYLOAD_OFFSET);
        }
        memset(reply.payload, 0, ADVC_HELLO_SIZE);
        uint64_t server_features = provider != NULL ? provider->feature_bits : 0;
        if (request.header.version_minor < 3)
            server_features &= ~ADVC_FEATURE_BROKER_EGL_SURFACE;
        if (request.header.version_minor < 4)
            server_features &= ~ADVC_FEATURE_ANDROID_AHB_SURFACE;
        if (request.header.version_minor < 5)
            server_features &= ~(ADVC_FEATURE_DMABUF |
                                 ADVC_FEATURE_DMABUF_EGL |
                                 ADVC_FEATURE_DMABUF_VULKAN);
        if (request.header.version_minor < 6)
            server_features &= ~ADVC_FEATURE_DECODE_PRIME;
        if (request.header.version_minor < 8)
            server_features &= ~(ADVC_FEATURE_ASYNC_DECODE_PRIME |
                                 ADVC_FEATURE_DECODE_QCOM_MODIFIER |
                                 ADVC_FEATURE_ENCODE_QCOM_MODIFIER);
        advc_put_u64(reply.payload + ADVC_HELLO_FEATURES_OFFSET,
                     server_features & client_features);
        advc_put_u32(reply.payload + ADVC_HELLO_MAX_PAYLOAD_OFFSET,
                     client_max_payload < ADVC_MAX_PAYLOAD ? client_max_payload : ADVC_MAX_PAYLOAD);
        reply.header.payload_size = ADVC_HELLO_SIZE;
        break;
    }
    case ADVC_OP_PING:
        status_reply(&reply, ADVC_STATUS_OK, 0);
        break;
    case ADVC_OP_QUERY_CAPABILITIES: {
        struct advc_capability_set caps;
        char error[128];
        memset(&caps, 0, sizeof(caps));
        memset(error, 0, sizeof(error));
        if (provider == NULL || provider->query_capabilities == NULL) {
            status_reply(&reply, ADVC_STATUS_UNSUPPORTED, 0);
        } else if (provider->query_capabilities(provider->userdata, &caps, error, sizeof(error)) < 0) {
            status_reply(&reply, ADVC_STATUS_CODEC_ERROR, 0);
        } else {
            caps.transport_features &= provider->feature_bits;
            if (request.header.version_minor < 3)
                caps.transport_features &= ~ADVC_FEATURE_BROKER_EGL_SURFACE;
            if (request.header.version_minor < 4)
                caps.transport_features &= ~ADVC_FEATURE_ANDROID_AHB_SURFACE;
            if (request.header.version_minor < 5)
                caps.transport_features &= ~(ADVC_FEATURE_DMABUF |
                                             ADVC_FEATURE_DMABUF_EGL |
                                             ADVC_FEATURE_DMABUF_VULKAN);
            if (request.header.version_minor < 6)
                caps.transport_features &= ~ADVC_FEATURE_DECODE_PRIME;
            if (request.header.version_minor < 8)
                caps.transport_features &=
                    ~(ADVC_FEATURE_ASYNC_DECODE_PRIME |
                      ADVC_FEATURE_DECODE_QCOM_MODIFIER |
                      ADVC_FEATURE_ENCODE_QCOM_MODIFIER);
            if (write_capability_reply(&reply, &caps) < 0)
                status_reply(&reply, ADVC_STATUS_CODEC_ERROR, 0);
        }
        break;
    }
    default:
        if (provider != NULL && provider->handle_codec_request != NULL) {
            uint32_t status = provider->handle_codec_request(provider->userdata, &request, &reply);
            if (status != ADVC_STATUS_OK && reply.header.payload_size == 0) status_reply(&reply, status, 0);
        } else {
            status_reply(&reply, ADVC_STATUS_UNSUPPORTED, 0);
        }
        break;
    }
send_reply:
    if (advc_send_message(client_fd, &reply) < 0) goto close_reply_fds;
    if (provider != NULL && provider->after_reply != NULL &&
        provider->after_reply(provider->userdata, client_fd, &request, &reply) < 0)
        goto close_reply_fds;
    result = 0;

close_reply_fds:
    advc_close_message_fds(&reply);
close_request_fds:
    advc_close_message_fds(&request);
done:
    free(reply_payload);
    free(request_payload);
    return result;
}
