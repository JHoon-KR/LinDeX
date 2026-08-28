#ifndef ADVC_BROKER_H
#define ADVC_BROKER_H

#include "advc/capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

struct advc_broker_provider {
    void *userdata;
    uint64_t feature_bits;
    int (*query_capabilities)(void *userdata, struct advc_capability_set *out,
                              char *error, size_t error_size);
    /* Reserved for the codec engine. Return an advc_status value. */
    uint32_t (*handle_codec_request)(void *userdata, const struct advc_message *request,
                                     struct advc_message *reply);
    int (*after_reply)(void *userdata, int client_fd,
                       const struct advc_message *request,
                       const struct advc_message *reply);
};

/* Handles one request and sends its reply. Returns 0, -1 on transport failure. */
int advc_broker_handle_once(int client_fd, const struct advc_broker_provider *provider);

/* Capability reply ABI. */
#define ADVC_CAPS_PREFIX_SIZE 24u
#define ADVC_CAPS_ENTRY_SIZE 224u
#define ADVC_CAPS_ENTRY_MIME_OFFSET 0u
#define ADVC_CAPS_ENTRY_NAME_OFFSET 64u
#define ADVC_CAPS_ENTRY_DIRECTION_OFFSET 192u
#define ADVC_CAPS_ENTRY_ACCELERATION_OFFSET 193u
#define ADVC_CAPS_ENTRY_LOW_LATENCY_OFFSET 194u
#define ADVC_CAPS_ENTRY_SECURE_OFFSET 195u
#define ADVC_CAPS_ENTRY_MAX_WIDTH_OFFSET 196u
#define ADVC_CAPS_ENTRY_MAX_HEIGHT_OFFSET 200u
#define ADVC_CAPS_ENTRY_MAX_FPS_MILLI_OFFSET 204u
#define ADVC_CAPS_ENTRY_FLAGS_OFFSET 208u

#ifdef __cplusplus
}
#endif

#endif
