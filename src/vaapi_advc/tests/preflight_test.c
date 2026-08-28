#include "advc_vaapi_preflight.h"
#include "advc_vaapi_policy.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void add_codec(struct advc_capability_set *caps, const char *mime,
                      const char *name, uint8_t acceleration) {
    struct advc_codec_capability *codec = &caps->codecs[caps->count++];
    memset(codec, 0, sizeof(*codec));
    strcpy(codec->mime, mime);
    strcpy(codec->codec_name, name);
    codec->direction = ADVC_DIRECTION_DECODE;
    codec->acceleration = acceleration;
    codec->max_width = 4096;
    codec->max_height = 2160;
}

int main(void) {
    const uint64_t features = ADVC_FEATURE_DECODE |
                              ADVC_FEATURE_DECODE_PRIME;
    struct advc_capability_set caps = {0};
    struct advc_vaapi_preflight_result result;

    add_codec(&caps, "video/hevc", "c2.qti.hevc.decoder",
              ADVC_ACCELERATION_HARDWARE);
    add_codec(&caps, "video/x-vnd.on2.vp9", "c2.qti.vp9.decoder",
              ADVC_ACCELERATION_HARDWARE);
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "hevc-main",
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN, &result) == 0);
    assert(result.codec_bit == ADVC_VAAPI_CODEC_HEVC_MAIN);
    assert(strcmp(result.codec_name, "c2.qti.hevc.decoder") == 0);
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "vp9-profile0",
               ADVC_VAAPI_VP9_PROFILE0_VALIDATION_TOKEN, &result) == 0);
    assert(strcmp(result.codec_name, "c2.qti.vp9.decoder") == 0);

    errno = 0;
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "hevc-main", "validated-main-v1",
               &result) < 0 && errno == EACCES);
    assert(advc_vaapi_preflight_evaluate(
               &caps, ADVC_FEATURE_DECODE, "hevc-main",
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN, &result) < 0 &&
           errno == ENOTSUP);
    caps.codecs[0].acceleration = ADVC_ACCELERATION_SOFTWARE;
    strcpy(caps.codecs[0].codec_name, "c2.android.hevc.decoder");
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "hevc-main",
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN, &result) < 0 &&
           errno == ENOTSUP);
    caps.codecs[0].acceleration = ADVC_ACCELERATION_HARDWARE;
    strcpy(caps.codecs[0].codec_name, "unknown.hevc.decoder");
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "hevc-main",
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN, &result) < 0 &&
           errno == ENOTSUP);
    assert(advc_vaapi_preflight_evaluate(
               &caps, features, "hevc-main10",
               ADVC_VAAPI_HEVC_MAIN_VALIDATION_TOKEN, &result) < 0 &&
           errno == EINVAL);
    return 0;
}
