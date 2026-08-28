#ifndef ADVC_VAAPI_PREFLIGHT_H
#define ADVC_VAAPI_PREFLIGHT_H

#include "advc/capabilities.h"

#include <stdint.h>

struct advc_vaapi_preflight_result {
    uint32_t codec_bit;
    uint32_t max_width;
    uint32_t max_height;
    char codec_name[ADVC_MAX_CODEC_NAME];
};

/*
 * Pure capability/token evaluation used by the session preflight executable.
 * profile_name is exactly "hevc-main" or "vp9-profile0". The supplied HELLO
 * feature mask is authoritative over the CAPS transport snapshot.
 */
int advc_vaapi_preflight_evaluate(
    const struct advc_capability_set *caps, uint64_t hello_features,
    const char *profile_name, const char *validation_token,
    struct advc_vaapi_preflight_result *result);

#endif
