#ifndef ADVC_VAAPI_POLICY_H
#define ADVC_VAAPI_POLICY_H

#include "advc/capabilities.h"

#include <stdint.h>
#include <va/va.h>

#ifdef __cplusplus
extern "C" {
#endif

enum advc_vaapi_codec_bits {
    ADVC_VAAPI_CODEC_H264 = UINT32_C(1) << 0,
    ADVC_VAAPI_CODEC_HEVC_MAIN = UINT32_C(1) << 1,
    ADVC_VAAPI_CODEC_VP9_PROFILE0 = UINT32_C(1) << 2,
};

#define ADVC_VAAPI_AVC_VALIDATION_TOKEN "validated-v1"
#define ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN \
    "validated-main-inter-prime-eos-120of120-v1"
#define ADVC_VAAPI_VP9_PROFILE0_VALIDATION_TOKEN \
    "validated-profile0-inter-prime-eos-120of120-v1"

struct advc_vaapi_policy {
    uint32_t broker_codecs;
    uint32_t advertised_codecs;
    uint32_t h264_max_width;
    uint32_t h264_max_height;
    uint32_t hevc_max_width;
    uint32_t hevc_max_height;
    uint32_t vp9_max_width;
    uint32_t vp9_max_height;
    char h264_codec_name[ADVC_MAX_CODEC_NAME];
    char hevc_codec_name[ADVC_MAX_CODEC_NAME];
    char vp9_codec_name[ADVC_MAX_CODEC_NAME];
};

/* Exact build/device-validation tokens; old or partial strings fail closed. */
int advc_vaapi_validation_token_matches(uint32_t codec_bit,
                                        const char *token);
uint32_t advc_vaapi_validation_mask(const char *avc_token,
                                    const char *hevc_token,
                                    const char *vp9_token);

/*
 * Translate authoritative broker capabilities into a Linux-facing policy.
 * Decode profiles stay unadvertised until the parsed VA-API buffer to complete
 * Annex-B access-unit translator is ready. This distinction is deliberate:
 * detecting a MediaCodec decoder is not proof that a VA-API decode pipeline is
 * usable.
 */
int advc_vaapi_policy_from_capabilities(
    const struct advc_capability_set *caps, uint32_t ready_codec_mask,
    struct advc_vaapi_policy *policy);

/* Single source of truth for profiles exposed to vainfo/FFmpeg/GStreamer. */
int advc_vaapi_policy_profile_advertised(
    const struct advc_vaapi_policy *policy, VAProfile profile);

#ifdef __cplusplus
}
#endif

#endif
