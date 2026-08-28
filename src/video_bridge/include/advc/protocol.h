#ifndef ADVC_PROTOCOL_H
#define ADVC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Android Debian Video Codec broker protocol, version 1.
 *
 * The wire format is little-endian and does not expose compiler structs.
 * Each packet is one AF_UNIX/SOCK_SEQPACKET record. File descriptors are
 * attached with SCM_RIGHTS and their semantic roles are carried in payloads.
 */
#define ADVC_MAGIC UINT32_C(0x43564441) /* "ADVC" in little endian */
#define ADVC_VERSION_MAJOR 1u
#define ADVC_VERSION_MINOR 8u
#define ADVC_HEADER_SIZE 32u
#define ADVC_MAX_PAYLOAD (1024u * 1024u)
#define ADVC_MAX_FDS 8u
#define ADVC_MAX_MIME 64u
#define ADVC_MAX_CODEC_NAME 128u

enum advc_message_type {
    ADVC_MSG_REQUEST = 1,
    ADVC_MSG_REPLY = 2,
    ADVC_MSG_EVENT = 3,
};

enum advc_opcode {
    ADVC_OP_HELLO = 1,
    ADVC_OP_QUERY_CAPABILITIES = 2,
    ADVC_OP_CREATE_SESSION = 3,
    ADVC_OP_QUEUE_INPUT = 4,
    ADVC_OP_DEQUEUE_OUTPUT = 5,
    ADVC_OP_RELEASE_OUTPUT = 6,
    ADVC_OP_FLUSH = 7,
    ADVC_OP_CLOSE_SESSION = 8,
    ADVC_OP_TRANSFER_AHB = 9,
    ADVC_OP_PING = 10,
    ADVC_OP_QUEUE_AHB = 11,
    ADVC_OP_COMPLETE_AHB = 12,
    ADVC_OP_REGISTER_DMABUF = 13,
    ADVC_OP_UNREGISTER_DMABUF = 14,
    ADVC_OP_QUEUE_DMABUF = 15,
    ADVC_OP_COMPLETE_DMABUF = 16,
    ADVC_OP_TRANSFER_PRIME = 17,
    /* Gateway-local decode reservation. Never forwarded to Android. */
    ADVC_OP_RESERVE_LINEAR = 18,
};

enum advc_status {
    ADVC_STATUS_OK = 0,
    ADVC_STATUS_BAD_MESSAGE = 1,
    ADVC_STATUS_UNSUPPORTED = 2,
    ADVC_STATUS_NO_RESOURCE = 3,
    ADVC_STATUS_CODEC_ERROR = 4,
    ADVC_STATUS_PERMISSION_DENIED = 5,
    ADVC_STATUS_WOULD_BLOCK = 6,
    ADVC_STATUS_INTERNAL = 7,
};

enum advc_codec_direction {
    ADVC_DIRECTION_DECODE = 1,
    ADVC_DIRECTION_ENCODE = 2,
};

enum advc_codec_acceleration {
    ADVC_ACCELERATION_UNKNOWN = 0,
    ADVC_ACCELERATION_SOFTWARE = 1,
    ADVC_ACCELERATION_HARDWARE = 2,
    ADVC_ACCELERATION_VENDOR_SOFTWARE = 3,
};

enum advc_buffer_transport {
    ADVC_TRANSPORT_BYTES = 1,       /* memfd payload, optional fence */
    ADVC_TRANSPORT_DMABUF = 2,      /* one or more dma-buf plane fds */
    ADVC_TRANSPORT_AHARDWAREBUFFER = 3,
    /* Empty frame-control packets; broker-local EGL renders into encoder Surface. */
    ADVC_TRANSPORT_BROKER_EGL_SURFACE = 4,
    /* Android-client AHB -> broker EGLImage -> encoder Surface. */
    ADVC_TRANSPORT_ANDROID_AHB_SURFACE = 5,
};

enum advc_fd_role {
    ADVC_FD_NONE = 0,
    ADVC_FD_INPUT_DATA = 1,
    ADVC_FD_OUTPUT_DATA = 2,
    ADVC_FD_DMABUF_PLANE_0 = 16,
    ADVC_FD_DMABUF_PLANE_1 = 17,
    ADVC_FD_DMABUF_PLANE_2 = 18,
    ADVC_FD_DMABUF_PLANE_3 = 19,
    ADVC_FD_ACQUIRE_FENCE = 32,
    ADVC_FD_RELEASE_FENCE = 33,
};

enum advc_packet_flags {
    ADVC_FLAG_END_OF_STREAM = 1u << 0,
    ADVC_FLAG_KEY_FRAME = 1u << 1,
    ADVC_FLAG_CODEC_CONFIG = 1u << 2,
    ADVC_FLAG_SECURE = 1u << 3,
    ADVC_FLAG_AHB_FOLLOWS = 1u << 4,
};

struct advc_header {
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t message_type;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t session_id;
    uint32_t flags;
    uint32_t payload_size;
    uint16_t fd_count;
    uint16_t reserved;
};

struct advc_message {
    struct advc_header header;
    uint8_t *payload;
    size_t payload_capacity;
    int fds[ADVC_MAX_FDS];
};

/* Stable payload layout helpers. All functions return bytes written/read, or -1. */
int advc_header_encode(uint8_t out[ADVC_HEADER_SIZE], const struct advc_header *header);
int advc_header_decode(struct advc_header *header, const uint8_t in[ADVC_HEADER_SIZE]);
int advc_header_validate(const struct advc_header *header);

/* Sends/receives exactly one SOCK_SEQPACKET record. Received fds use CLOEXEC. */
int advc_send_message(int socket_fd, const struct advc_message *message);
int advc_receive_message(int socket_fd, struct advc_message *message);
void advc_close_message_fds(struct advc_message *message);

void advc_put_u16(uint8_t *p, uint16_t value);
void advc_put_u32(uint8_t *p, uint32_t value);
void advc_put_u64(uint8_t *p, uint64_t value);
uint16_t advc_get_u16(const uint8_t *p);
uint32_t advc_get_u32(const uint8_t *p);
uint64_t advc_get_u64(const uint8_t *p);

/*
 * Common payload offsets. These constants are the protocol ABI; C structs are
 * intentionally avoided so Android and glibc clients cannot disagree on padding.
 */
enum {
    ADVC_HELLO_SIZE = 16,
    ADVC_HELLO_FEATURES_OFFSET = 0,
    ADVC_HELLO_MAX_PAYLOAD_OFFSET = 8,

    ADVC_STATUS_SIZE = 8,
    ADVC_STATUS_CODE_OFFSET = 0,
    ADVC_STATUS_DETAIL_OFFSET = 4,

    ADVC_CREATE_V16_SIZE = 96,
    ADVC_CREATE_SIZE = 100,
    ADVC_CREATE_DIRECTION_OFFSET = 0,
    ADVC_CREATE_WIDTH_OFFSET = 4,
    ADVC_CREATE_HEIGHT_OFFSET = 8,
    ADVC_CREATE_BITRATE_OFFSET = 12,
    ADVC_CREATE_FRAMERATE_MILLI_OFFSET = 16,
    ADVC_CREATE_FLAGS_OFFSET = 20,
    ADVC_CREATE_MIME_OFFSET = 24,
    /* Version 1.1 encode extension. Decode requests must leave this zero. */
    ADVC_CREATE_COLOR_FORMAT_OFFSET = 88,
    /* Version 1.2: exact requested output/input transport. */
    ADVC_CREATE_TRANSPORT_OFFSET = 92,
    /* Version 1.7: exact encoder profile; decode must leave this zero. */
    ADVC_CREATE_ENCODE_PROFILE_OFFSET = 96,

    ADVC_QUEUE_INPUT_SIZE = 48,
    ADVC_QUEUE_INPUT_BUFFER_ID_OFFSET = 0,
    ADVC_QUEUE_INPUT_PTS_NS_OFFSET = 8,
    ADVC_QUEUE_INPUT_DATA_OFFSET = 16,
    ADVC_QUEUE_INPUT_SIZE_OFFSET = 24,
    ADVC_QUEUE_INPUT_FLAGS_OFFSET = 32,
    ADVC_QUEUE_INPUT_FD_ROLE_OFFSET = 36,

    ADVC_OUTPUT_READY_SIZE = 112,
    ADVC_OUTPUT_BUFFER_ID_OFFSET = 0,
    ADVC_OUTPUT_PTS_NS_OFFSET = 8,
    ADVC_OUTPUT_SIZE_OFFSET = 16,
    ADVC_OUTPUT_FLAGS_OFFSET = 24,
    ADVC_OUTPUT_TRANSPORT_OFFSET = 28,
    ADVC_OUTPUT_WIDTH_OFFSET = 32,
    ADVC_OUTPUT_HEIGHT_OFFSET = 36,
    ADVC_OUTPUT_ANDROID_FORMAT_OFFSET = 40,
    ADVC_OUTPUT_DRM_FOURCC_OFFSET = 44,
    ADVC_OUTPUT_DRM_MODIFIER_OFFSET = 48,
    ADVC_OUTPUT_LAYERS_OFFSET = 56,
    ADVC_OUTPUT_STRIDE_OFFSET = 60,
    ADVC_OUTPUT_USAGE_OFFSET = 64,
    ADVC_OUTPUT_PLANE_COUNT_OFFSET = 72,
    ADVC_OUTPUT_PLANE_OFFSETS_OFFSET = 76,
    ADVC_OUTPUT_PLANE_STRIDES_OFFSET = 92,
    ADVC_OUTPUT_ACQUIRE_FENCE_ROLE_OFFSET = 108,

    /* Version 1.1 byte-transport extension appended to OUTPUT_READY. */
    ADVC_OUTPUT_BYTES_SIZE = 136,
    ADVC_OUTPUT_SLICE_HEIGHT_OFFSET = 112,
    ADVC_OUTPUT_CROP_LEFT_OFFSET = 116,
    ADVC_OUTPUT_CROP_TOP_OFFSET = 120,
    ADVC_OUTPUT_CROP_RIGHT_OFFSET = 124,
    ADVC_OUTPUT_CROP_BOTTOM_OFFSET = 128,

    ADVC_RELEASE_OUTPUT_SIZE = 16,
    ADVC_RELEASE_OUTPUT_BUFFER_ID_OFFSET = 0,
    ADVC_RELEASE_OUTPUT_FENCE_ROLE_OFFSET = 8,

    ADVC_TRANSFER_AHB_SIZE = 8,
    ADVC_TRANSFER_AHB_BUFFER_ID_OFFSET = 0,

    ADVC_QUEUE_AHB_SIZE = 48,
    ADVC_QUEUE_AHB_PTS_NS_OFFSET = 0,
    ADVC_QUEUE_AHB_WIDTH_OFFSET = 8,
    ADVC_QUEUE_AHB_HEIGHT_OFFSET = 12,
    ADVC_QUEUE_AHB_FORMAT_OFFSET = 16,
    ADVC_QUEUE_AHB_LAYERS_OFFSET = 20,
    ADVC_QUEUE_AHB_USAGE_OFFSET = 24,
    ADVC_QUEUE_AHB_FLAGS_OFFSET = 32,
    ADVC_QUEUE_AHB_FENCE_ROLE_OFFSET = 36,

    ADVC_COMPLETE_AHB_SIZE = 16,
    ADVC_COMPLETE_AHB_STATUS_OFFSET = 0,
    ADVC_COMPLETE_AHB_DETAIL_OFFSET = 4,
    ADVC_COMPLETE_AHB_FENCE_ROLE_OFFSET = 8,

    /* Version 1.5 explicit DRM PRIME registration contract. */
    ADVC_REGISTER_DMABUF_SIZE = 256,
    ADVC_REGISTER_DMABUF_BUFFER_ID_OFFSET = 0,
    ADVC_REGISTER_DMABUF_WIDTH_OFFSET = 8,
    ADVC_REGISTER_DMABUF_HEIGHT_OFFSET = 12,
    ADVC_REGISTER_DMABUF_FOURCC_OFFSET = 16,
    ADVC_REGISTER_DMABUF_FLAGS_OFFSET = 20,
    ADVC_REGISTER_DMABUF_MODIFIER_OFFSET = 24,
    ADVC_REGISTER_DMABUF_CROP_LEFT_OFFSET = 32,
    ADVC_REGISTER_DMABUF_CROP_TOP_OFFSET = 36,
    ADVC_REGISTER_DMABUF_CROP_WIDTH_OFFSET = 40,
    ADVC_REGISTER_DMABUF_CROP_HEIGHT_OFFSET = 44,
    ADVC_REGISTER_DMABUF_OBJECT_COUNT_OFFSET = 48,
    ADVC_REGISTER_DMABUF_PLANE_COUNT_OFFSET = 52,
    ADVC_REGISTER_DMABUF_COLOR_PRIMARIES_OFFSET = 56,
    ADVC_REGISTER_DMABUF_COLOR_TRANSFER_OFFSET = 60,
    ADVC_REGISTER_DMABUF_COLOR_MATRIX_OFFSET = 64,
    ADVC_REGISTER_DMABUF_COLOR_RANGE_OFFSET = 68,
    ADVC_REGISTER_DMABUF_CHROMA_HORIZONTAL_OFFSET = 72,
    ADVC_REGISTER_DMABUF_CHROMA_VERTICAL_OFFSET = 76,
    ADVC_REGISTER_DMABUF_OBJECTS_OFFSET = 96,
    ADVC_REGISTER_DMABUF_OBJECT_STRIDE = 16,
    ADVC_REGISTER_DMABUF_OBJECT_FD_INDEX_OFFSET = 0,
    ADVC_REGISTER_DMABUF_OBJECT_SIZE_OFFSET = 8,
    ADVC_REGISTER_DMABUF_PLANES_OFFSET = 160,
    ADVC_REGISTER_DMABUF_PLANE_STRIDE = 24,
    ADVC_REGISTER_DMABUF_PLANE_OBJECT_INDEX_OFFSET = 0,
    ADVC_REGISTER_DMABUF_PLANE_OFFSET_OFFSET = 8,
    ADVC_REGISTER_DMABUF_PLANE_PITCH_OFFSET = 16,

    ADVC_UNREGISTER_DMABUF_SIZE = 8,
    ADVC_UNREGISTER_DMABUF_BUFFER_ID_OFFSET = 0,

    ADVC_QUEUE_DMABUF_SIZE = 32,
    ADVC_QUEUE_DMABUF_BUFFER_ID_OFFSET = 0,
    ADVC_QUEUE_DMABUF_PTS_NS_OFFSET = 8,
    ADVC_QUEUE_DMABUF_FLAGS_OFFSET = 16,
    ADVC_QUEUE_DMABUF_FENCE_ROLE_OFFSET = 20,

    /* Client completion query: the exact in-flight buffer ID. */
    ADVC_COMPLETE_DMABUF_REQUEST_SIZE = 8,
    ADVC_COMPLETE_DMABUF_REQUEST_BUFFER_ID_OFFSET = 0,

    ADVC_COMPLETE_DMABUF_SIZE = 24,
    ADVC_COMPLETE_DMABUF_BUFFER_ID_OFFSET = 0,
    ADVC_COMPLETE_DMABUF_STATUS_OFFSET = 8,
    ADVC_COMPLETE_DMABUF_DETAIL_OFFSET = 12,
    ADVC_COMPLETE_DMABUF_FENCE_ROLE_OFFSET = 16,

    /* Version 1.6 decoded AHB -> authoritative DRM PRIME transfer. */
    ADVC_TRANSFER_PRIME_SIZE = 8,
    ADVC_TRANSFER_PRIME_BUFFER_ID_OFFSET = 0,
    ADVC_TRANSFER_PRIME_REPLY_SIZE = ADVC_STATUS_SIZE + ADVC_REGISTER_DMABUF_SIZE,
    ADVC_TRANSFER_PRIME_DESCRIPTOR_OFFSET = ADVC_STATUS_SIZE,

    /* Version 1.8: reserve a LINEAR NV12 destination before decode output. */
    ADVC_RESERVE_LINEAR_SIZE = 24,
    ADVC_RESERVE_LINEAR_PTS_NS_OFFSET = 0,
    ADVC_RESERVE_LINEAR_WIDTH_OFFSET = 8,
    ADVC_RESERVE_LINEAR_HEIGHT_OFFSET = 12,
    ADVC_RESERVE_LINEAR_FOURCC_OFFSET = 16,
    ADVC_RESERVE_LINEAR_REPLY_SIZE = ADVC_TRANSFER_PRIME_REPLY_SIZE,
};

/* Version 1.7 encode-session profile contract. */
enum advc_encode_profile {
    ADVC_ENCODE_PROFILE_NONE = 0,
    ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE = 1,
    ADVC_ENCODE_PROFILE_H264_MAIN = 2,
    ADVC_ENCODE_PROFILE_H264_HIGH = 3,
    ADVC_ENCODE_PROFILE_HEVC_MAIN = 4,
};

/* Hard protocol resource bounds shared by the broker and its clients. */
#define ADVC_MAX_SESSIONS 4u
#define ADVC_MAX_OUTSTANDING_OUTPUTS 8u
#define ADVC_MAX_INPUT_BYTES (16u * 1024u * 1024u)
#define ADVC_MAX_OUTPUT_BYTES (16u * 1024u * 1024u)
#define ADVC_MAX_ENCODE_BITRATE 200000000u
#define ADVC_MAX_REGISTERED_DMABUFS 16u
#define ADVC_MAX_INFLIGHT_DMABUFS 4u
#define ADVC_MAX_DMABUF_OBJECTS 4u
#define ADVC_MAX_DMABUF_PLANES 4u
#define ADVC_MAX_DMABUF_OBJECT_BYTES (UINT64_C(1) << 30)
#define ADVC_MAX_DMABUF_PITCH (1u << 20)

#define ADVC_DMABUF_EXPLICIT_FOURCC (UINT32_C(1) << 0)
#define ADVC_DMABUF_EXPLICIT_MODIFIER (UINT32_C(1) << 1)
#define ADVC_DMABUF_EXPLICIT_PLANES (UINT32_C(1) << 2)
#define ADVC_DMABUF_EXPLICIT_ALL                                           \
    (ADVC_DMABUF_EXPLICIT_FOURCC | ADVC_DMABUF_EXPLICIT_MODIFIER |          \
     ADVC_DMABUF_EXPLICIT_PLANES)

/* Android COLOR_Format values accepted by the bounded byte-buffer encoder. */
#define ADVC_COLOR_FORMAT_YUV420_PLANAR 19u
#define ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR 21u

enum advc_feature_bits {
    ADVC_FEATURE_MEMFD = UINT64_C(1) << 0,
    ADVC_FEATURE_DMABUF = UINT64_C(1) << 1,
    ADVC_FEATURE_NATIVE_FENCE = UINT64_C(1) << 2,
    ADVC_FEATURE_AHARDWAREBUFFER = UINT64_C(1) << 3,
    ADVC_FEATURE_DECODE = UINT64_C(1) << 4,
    ADVC_FEATURE_ENCODE = UINT64_C(1) << 5,
    ADVC_FEATURE_BROKER_EGL_SURFACE = UINT64_C(1) << 6,
    ADVC_FEATURE_ANDROID_AHB_SURFACE = UINT64_C(1) << 7,
    /* Backend-specific real-probe bits; DMABUF remains their aggregate gate. */
    ADVC_FEATURE_DMABUF_EGL = UINT64_C(1) << 8,
    ADVC_FEATURE_DMABUF_VULKAN = UINT64_C(1) << 9,
    /* Decode AHB -> authoritative DRM PRIME; real-probe gated and off by default. */
    ADVC_FEATURE_DECODE_PRIME = UINT64_C(1) << 10,
    /* Gateway can return an empty LINEAR surface before decoded output. */
    ADVC_FEATURE_ASYNC_DECODE_PRIME = UINT64_C(1) << 11,
    /* Modifier-preserving QCOM decode PRIME export passed its broker gate. */
    ADVC_FEATURE_DECODE_QCOM_MODIFIER = UINT64_C(1) << 12,
    /* Vulkan encoder ingress passed its QCOM modifier import gate. */
    ADVC_FEATURE_ENCODE_QCOM_MODIFIER = UINT64_C(1) << 13,
};

#ifdef __cplusplus
}
#endif

#endif
