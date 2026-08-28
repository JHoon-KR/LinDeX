#ifndef ADVC_CAPABILITIES_H
#define ADVC_CAPABILITIES_H

#include "advc/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADVC_MAX_CAPABILITIES 32u

struct advc_codec_capability {
    char mime[ADVC_MAX_MIME];
    char codec_name[ADVC_MAX_CODEC_NAME];
    uint8_t direction;
    uint8_t acceleration;
    uint8_t low_latency;
    uint8_t secure_playback;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_fps_milli;
    uint32_t flags;
};

struct advc_capability_set {
    uint32_t api_level;
    uint32_t count;
    uint64_t transport_features;
    struct advc_codec_capability codecs[ADVC_MAX_CAPABILITIES];
};

/* Android implementation probes known video MIME types using libmediandk. */
int advc_probe_android_capabilities(struct advc_capability_set *out, char *error, size_t error_size);

/*
 * Fail-closed component-name evidence used after MediaCodec has selected a
 * component by MIME. Unknown, secure, or software-marked names never become a
 * hardware capability. Selection itself does not depend on this allowlist.
 */
uint8_t advc_classify_codec_component(const char *name);

/* Portable JSON writer used by the command-line probe and host tests. */
int advc_capabilities_write_json(const struct advc_capability_set *caps, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
