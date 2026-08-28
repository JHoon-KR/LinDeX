#include "advc_vaapi_preflight.h"

#include "advc_vaapi_policy.h"

#include <errno.h>
#include <string.h>

static int profile_codec_bit(const char *profile_name,
                             uint32_t *codec_bit) {
    if (profile_name == NULL || codec_bit == NULL) return -1;
    if (strcmp(profile_name, "hevc-main") == 0) {
        *codec_bit = ADVC_VAAPI_CODEC_HEVC_MAIN;
        return 0;
    }
    if (strcmp(profile_name, "vp9-profile0") == 0) {
        *codec_bit = ADVC_VAAPI_CODEC_VP9_PROFILE0;
        return 0;
    }
    return -1;
}

int advc_vaapi_preflight_evaluate(
    const struct advc_capability_set *caps, uint64_t hello_features,
    const char *profile_name, const char *validation_token,
    struct advc_vaapi_preflight_result *result) {
    struct advc_capability_set authoritative;
    struct advc_vaapi_policy policy;
    const char *codec_name;
    uint32_t codec_bit;
    uint32_t max_width;
    uint32_t max_height;
    size_t codec_name_length;

    if (caps == NULL || result == NULL ||
        caps->count > ADVC_MAX_CAPABILITIES ||
        profile_codec_bit(profile_name, &codec_bit) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!advc_vaapi_validation_token_matches(codec_bit,
                                             validation_token)) {
        errno = EACCES;
        return -1;
    }
    authoritative = *caps;
    authoritative.transport_features = hello_features;
    if (advc_vaapi_policy_from_capabilities(&authoritative, codec_bit,
                                            &policy) < 0)
        return -1;
    if ((policy.advertised_codecs & codec_bit) == 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (codec_bit == ADVC_VAAPI_CODEC_HEVC_MAIN) {
        codec_name = policy.hevc_codec_name;
        max_width = policy.hevc_max_width;
        max_height = policy.hevc_max_height;
    } else {
        codec_name = policy.vp9_codec_name;
        max_width = policy.vp9_max_width;
        max_height = policy.vp9_max_height;
    }
    codec_name_length = strnlen(codec_name, sizeof(result->codec_name));
    if (codec_name_length == 0u ||
        codec_name_length >= sizeof(result->codec_name)) {
        errno = ENOTSUP;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->codec_bit = codec_bit;
    result->max_width = max_width;
    result->max_height = max_height;
    memcpy(result->codec_name, codec_name, codec_name_length + 1u);
    return 0;
}
