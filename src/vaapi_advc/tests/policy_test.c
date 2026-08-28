#include "advc_vaapi_policy.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct advc_codec_capability *add_codec(
    struct advc_capability_set *caps, const char *mime, uint8_t direction,
    uint8_t acceleration, uint32_t width, uint32_t height) {
    struct advc_codec_capability *codec = &caps->codecs[caps->count++];

    memset(codec, 0, sizeof(*codec));
    assert(strlen(mime) < sizeof(codec->mime));
    strcpy(codec->mime, mime);
    if (acceleration == ADVC_ACCELERATION_HARDWARE) {
        if (strcmp(mime, "video/avc") == 0)
            strcpy(codec->codec_name, "c2.qti.avc.decoder");
        else if (strcmp(mime, "video/hevc") == 0)
            strcpy(codec->codec_name, "c2.qti.hevc.decoder");
        else if (strcmp(mime, "video/x-vnd.on2.vp9") == 0)
            strcpy(codec->codec_name, "c2.qti.vp9.decoder");
        else
            strcpy(codec->codec_name, "c2.qti.other.decoder");
    } else {
        strcpy(codec->codec_name, "c2.android.decoder");
    }
    codec->direction = direction;
    codec->acceleration = acceleration;
    codec->max_width = width;
    codec->max_height = height;
    return codec;
}

static void test_fail_closed_feature_gate(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;

    caps.transport_features = ADVC_FEATURE_DECODE;
    add_codec(&caps, "video/avc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 3840, 2160);
    assert(advc_vaapi_policy_from_capabilities(
               &caps, ADVC_VAAPI_CODEC_H264, &policy) == 0);
    assert(policy.broker_codecs == 0);
    assert(policy.advertised_codecs == 0);
}

static void test_hardware_and_translator_gates(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;

    caps.transport_features = ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_DECODE_PRIME;
    add_codec(&caps, "video/avc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 3840, 2160);
    add_codec(&caps, "video/avc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_SOFTWARE, 7680, 4320);
    add_codec(&caps, "video/hevc", ADVC_DIRECTION_ENCODE,
              ADVC_ACCELERATION_HARDWARE, 7680, 4320);
    add_codec(&caps, "video/hevc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 4096, 2304);
    add_codec(&caps, "video/x-vnd.on2.vp9", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 7680, 4320);
    add_codec(&caps, "video/av01", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 8192, 4320);

    assert(advc_vaapi_policy_from_capabilities(&caps, 0, &policy) == 0);
    assert(policy.broker_codecs ==
           (ADVC_VAAPI_CODEC_H264 | ADVC_VAAPI_CODEC_HEVC_MAIN |
            ADVC_VAAPI_CODEC_VP9_PROFILE0));
    assert(policy.advertised_codecs == 0);
    assert(policy.h264_max_width == 3840 &&
           policy.h264_max_height == 2160);
    assert(policy.hevc_max_width == 4096 &&
           policy.hevc_max_height == 2304);
    assert(policy.vp9_max_width == 7680 &&
           policy.vp9_max_height == 4320);
    assert(strcmp(policy.h264_codec_name, "c2.qti.avc.decoder") == 0);
    assert(strcmp(policy.hevc_codec_name, "c2.qti.hevc.decoder") == 0);
    assert(strcmp(policy.vp9_codec_name, "c2.qti.vp9.decoder") == 0);

    assert(advc_vaapi_policy_from_capabilities(
               &caps, ADVC_VAAPI_CODEC_H264, &policy) == 0);
    assert(policy.advertised_codecs == ADVC_VAAPI_CODEC_H264);

    assert(advc_vaapi_policy_from_capabilities(
               &caps,
               ADVC_VAAPI_CODEC_HEVC_MAIN |
                   ADVC_VAAPI_CODEC_VP9_PROFILE0,
               &policy) == 0);
    assert(policy.advertised_codecs ==
           (ADVC_VAAPI_CODEC_HEVC_MAIN |
            ADVC_VAAPI_CODEC_VP9_PROFILE0));
}

static void test_exact_validation_tokens(void) {
    assert(advc_vaapi_validation_mask(
               ADVC_VAAPI_AVC_VALIDATION_TOKEN,
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN,
               ADVC_VAAPI_VP9_PROFILE0_VALIDATION_TOKEN) ==
           (ADVC_VAAPI_CODEC_H264 | ADVC_VAAPI_CODEC_HEVC_MAIN |
            ADVC_VAAPI_CODEC_VP9_PROFILE0));
    assert(!advc_vaapi_validation_token_matches(
        ADVC_VAAPI_CODEC_HEVC_MAIN, "validated-main-v1"));
    assert(!advc_vaapi_validation_token_matches(
        ADVC_VAAPI_CODEC_VP9_PROFILE0,
        "validated-profile0-inter-v1"));
    assert(!advc_vaapi_validation_token_matches(
        ADVC_VAAPI_CODEC_HEVC_MAIN | ADVC_VAAPI_CODEC_VP9_PROFILE0,
        ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN));
    assert(advc_vaapi_validation_mask(NULL, "wrong", "") == 0);
}

static void test_unknown_component_name_fails_closed(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;
    struct advc_codec_capability *codec;

    caps.transport_features = ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_DECODE_PRIME;
    codec = add_codec(&caps, "video/hevc", ADVC_DIRECTION_DECODE,
                      ADVC_ACCELERATION_HARDWARE, 3840, 2160);
    strcpy(codec->codec_name, "unknown.vendor.hevc.decoder");
    assert(advc_vaapi_policy_from_capabilities(
               &caps, ADVC_VAAPI_CODEC_HEVC_MAIN, &policy) == 0);
    assert(policy.broker_codecs == 0);
    assert(policy.advertised_codecs == 0);
}

static void test_invalid_inputs(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;

    errno = 0;
    assert(advc_vaapi_policy_from_capabilities(NULL, 0, &policy) < 0);
    assert(errno == EINVAL);
    caps.count = ADVC_MAX_CAPABILITIES + 1;
    assert(advc_vaapi_policy_from_capabilities(&caps, 0, &policy) < 0);
}

static void test_exact_va_profile_set(void) {
    struct advc_vaapi_policy policy = {
        .advertised_codecs = ADVC_VAAPI_CODEC_H264 |
                             ADVC_VAAPI_CODEC_HEVC_MAIN |
                             ADVC_VAAPI_CODEC_VP9_PROFILE0,
    };

    assert(advc_vaapi_policy_profile_advertised(
        &policy, VAProfileH264ConstrainedBaseline));
    assert(advc_vaapi_policy_profile_advertised(
        &policy, VAProfileH264Main));
    assert(advc_vaapi_policy_profile_advertised(
        &policy, VAProfileHEVCMain));
    assert(advc_vaapi_policy_profile_advertised(
        &policy, VAProfileVP9Profile0));

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileH264Baseline));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileH264High));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileHEVCMain10));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileVP9Profile1));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileVP9Profile2));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileVP9Profile3));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileAV1Profile0));
    assert(!advc_vaapi_policy_profile_advertised(NULL,
                                                 VAProfileH264Main));

    policy.advertised_codecs = ADVC_VAAPI_CODEC_HEVC_MAIN;
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileH264Main));
    assert(advc_vaapi_policy_profile_advertised(
        &policy, VAProfileHEVCMain));
    assert(!advc_vaapi_policy_profile_advertised(
        &policy, VAProfileVP9Profile0));
}

static void test_secure_capability_is_not_advertised(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;
    struct advc_codec_capability *codec;

    caps.transport_features = ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_DECODE_PRIME;
    codec = add_codec(&caps, "video/avc", ADVC_DIRECTION_DECODE,
                      ADVC_ACCELERATION_HARDWARE, 3840, 2160);
    codec->secure_playback = 1;
    assert(advc_vaapi_policy_from_capabilities(
               &caps, ADVC_VAAPI_CODEC_H264, &policy) == 0);
    assert(policy.broker_codecs == 0);
    assert(policy.advertised_codecs == 0);
}

static void test_missing_dimension_metadata_is_conservative(void) {
    struct advc_capability_set caps = {0};
    struct advc_vaapi_policy policy;

    caps.transport_features = ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_DECODE_PRIME;
    add_codec(&caps, "video/avc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 0, 0);
    add_codec(&caps, "video/hevc", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 0, 0);
    add_codec(&caps, "video/x-vnd.on2.vp9", ADVC_DIRECTION_DECODE,
              ADVC_ACCELERATION_HARDWARE, 0, 0);
    assert(advc_vaapi_policy_from_capabilities(
               &caps,
               ADVC_VAAPI_CODEC_H264 | ADVC_VAAPI_CODEC_HEVC_MAIN |
                   ADVC_VAAPI_CODEC_VP9_PROFILE0,
               &policy) == 0);
    assert(policy.broker_codecs ==
           (ADVC_VAAPI_CODEC_H264 | ADVC_VAAPI_CODEC_HEVC_MAIN |
            ADVC_VAAPI_CODEC_VP9_PROFILE0));
    assert(policy.advertised_codecs ==
           (ADVC_VAAPI_CODEC_H264 | ADVC_VAAPI_CODEC_HEVC_MAIN |
            ADVC_VAAPI_CODEC_VP9_PROFILE0));
    assert(policy.h264_max_width == 8192);
    assert(policy.h264_max_height == 8192);
    assert(policy.hevc_max_width == 8192);
    assert(policy.hevc_max_height == 8192);
    assert(policy.vp9_max_width == 8192);
    assert(policy.vp9_max_height == 8192);
}

int main(void) {
    test_fail_closed_feature_gate();
    test_hardware_and_translator_gates();
    test_exact_va_profile_set();
    test_secure_capability_is_not_advertised();
    test_missing_dimension_metadata_is_conservative();
    test_exact_validation_tokens();
    test_unknown_component_name_fails_closed();
    test_invalid_inputs();
    puts("advc VA-API capability policy: PASS");
    return 0;
}
