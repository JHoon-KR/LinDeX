#include "advc_vaapi_policy.h"

#include <errno.h>
#include <string.h>

static void merge_dimensions(uint32_t *width, uint32_t *height,
                             const struct advc_codec_capability *codec) {
    if (codec->max_width > *width) *width = codec->max_width;
    if (codec->max_height > *height) *height = codec->max_height;
}

int advc_vaapi_validation_token_matches(uint32_t codec_bit,
                                        const char *token) {
    const char *expected = NULL;
    if (codec_bit == ADVC_VAAPI_CODEC_H264)
        expected = ADVC_VAAPI_AVC_VALIDATION_TOKEN;
    else if (codec_bit == ADVC_VAAPI_CODEC_HEVC_MAIN)
        expected = ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN;
    else if (codec_bit == ADVC_VAAPI_CODEC_VP9_PROFILE0)
        expected = ADVC_VAAPI_VP9_PROFILE0_VALIDATION_TOKEN;
    return expected != NULL && token != NULL &&
           strcmp(token, expected) == 0;
}

uint32_t advc_vaapi_validation_mask(const char *avc_token,
                                    const char *hevc_token,
                                    const char *vp9_token) {
    uint32_t mask = 0;
    if (advc_vaapi_validation_token_matches(ADVC_VAAPI_CODEC_H264,
                                            avc_token))
        mask |= ADVC_VAAPI_CODEC_H264;
    if (advc_vaapi_validation_token_matches(ADVC_VAAPI_CODEC_HEVC_MAIN,
                                            hevc_token))
        mask |= ADVC_VAAPI_CODEC_HEVC_MAIN;
    if (advc_vaapi_validation_token_matches(ADVC_VAAPI_CODEC_VP9_PROFILE0,
                                            vp9_token))
        mask |= ADVC_VAAPI_CODEC_VP9_PROFILE0;
    return mask;
}

static int hardware_component_is_exact(
    const struct advc_codec_capability *codec) {
    return codec->codec_name[0] != '\0' &&
           memchr(codec->codec_name, '\0', sizeof(codec->codec_name)) != NULL &&
           advc_classify_codec_component(codec->codec_name) ==
               ADVC_ACCELERATION_HARDWARE;
}

static void retain_selected_name(char destination[ADVC_MAX_CODEC_NAME],
                                 const char *source) {
    if (destination[0] != '\0') return;
    strncpy(destination, source, ADVC_MAX_CODEC_NAME - 1u);
    destination[ADVC_MAX_CODEC_NAME - 1u] = '\0';
}

int advc_vaapi_policy_from_capabilities(
    const struct advc_capability_set *caps, uint32_t ready_codec_mask,
    struct advc_vaapi_policy *policy) {
    uint32_t i;

    if (caps == NULL || policy == NULL || caps->count > ADVC_MAX_CAPABILITIES) {
        errno = EINVAL;
        return -1;
    }

    memset(policy, 0, sizeof(*policy));
    if ((caps->transport_features & ADVC_FEATURE_DECODE) == 0 ||
        (caps->transport_features & ADVC_FEATURE_DECODE_PRIME) == 0)
        return 0;

    for (i = 0; i < caps->count; ++i) {
        const struct advc_codec_capability *codec = &caps->codecs[i];

        if (codec->direction != ADVC_DIRECTION_DECODE ||
            codec->acceleration != ADVC_ACCELERATION_HARDWARE ||
            codec->secure_playback != 0 ||
            !hardware_component_is_exact(codec))
            continue;

        if (strcmp(codec->mime, "video/avc") == 0) {
            policy->broker_codecs |= ADVC_VAAPI_CODEC_H264;
            merge_dimensions(&policy->h264_max_width,
                             &policy->h264_max_height, codec);
            retain_selected_name(policy->h264_codec_name,
                                 codec->codec_name);
        } else if (strcmp(codec->mime, "video/hevc") == 0) {
            policy->broker_codecs |= ADVC_VAAPI_CODEC_HEVC_MAIN;
            merge_dimensions(&policy->hevc_max_width,
                             &policy->hevc_max_height, codec);
            retain_selected_name(policy->hevc_codec_name,
                                 codec->codec_name);
        } else if (strcmp(codec->mime, "video/x-vnd.on2.vp9") == 0) {
            policy->broker_codecs |= ADVC_VAAPI_CODEC_VP9_PROFILE0;
            merge_dimensions(&policy->vp9_max_width,
                             &policy->vp9_max_height, codec);
            retain_selected_name(policy->vp9_codec_name,
                                 codec->codec_name);
        }
    }

    /*
     * Some MediaCodec capability probes report an authoritative hardware
     * codec name but omit the optional maximum-size metadata. Keep that codec
     * behind ready_codec_mask and use the protocol's bounded 8192-pixel cap;
     * CreateSession remains the authoritative per-stream gate and may reject
     * any request below that query-only upper bound.
     */
    if ((policy->broker_codecs & ADVC_VAAPI_CODEC_H264) != 0 &&
        (policy->h264_max_width == 0 || policy->h264_max_height == 0)) {
        policy->h264_max_width = 8192;
        policy->h264_max_height = 8192;
    }
    if ((policy->broker_codecs & ADVC_VAAPI_CODEC_HEVC_MAIN) != 0 &&
        (policy->hevc_max_width == 0 || policy->hevc_max_height == 0)) {
        policy->hevc_max_width = 8192;
        policy->hevc_max_height = 8192;
    }
    if ((policy->broker_codecs & ADVC_VAAPI_CODEC_VP9_PROFILE0) != 0 &&
        (policy->vp9_max_width == 0 || policy->vp9_max_height == 0)) {
        policy->vp9_max_width = 8192;
        policy->vp9_max_height = 8192;
    }

    policy->advertised_codecs = policy->broker_codecs & ready_codec_mask;
    return 0;
}

int advc_vaapi_policy_profile_advertised(
    const struct advc_vaapi_policy *policy, VAProfile profile) {
    if (policy == NULL) return 0;
    if (profile == VAProfileH264ConstrainedBaseline ||
        profile == VAProfileH264Main)
        return (policy->advertised_codecs & ADVC_VAAPI_CODEC_H264) != 0;
    if (profile == VAProfileHEVCMain)
        return (policy->advertised_codecs &
                ADVC_VAAPI_CODEC_HEVC_MAIN) != 0;
    if (profile == VAProfileVP9Profile0)
        return (policy->advertised_codecs &
                ADVC_VAAPI_CODEC_VP9_PROFILE0) != 0;
    return 0;
}
