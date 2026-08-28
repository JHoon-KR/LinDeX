#include "advc/capabilities.h"
#include "surface_encode_probe.h"
#include "android_ahb_socket_probe.h"

#include <android/api-level.h>
#include <media/NdkMediaCodec.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef media_status_t (*get_name_fn)(AMediaCodec *, char **);
typedef void (*release_name_fn)(AMediaCodec *, char *);

struct mime_probe {
    const char *mime;
    uint8_t direction;
};

static const struct mime_probe probes[] = {
    { "video/avc", ADVC_DIRECTION_DECODE },
    { "video/hevc", ADVC_DIRECTION_DECODE },
    { "video/x-vnd.on2.vp9", ADVC_DIRECTION_DECODE },
    { "video/av01", ADVC_DIRECTION_DECODE },
    { "video/avc", ADVC_DIRECTION_ENCODE },
    { "video/hevc", ADVC_DIRECTION_ENCODE },
    { "video/x-vnd.on2.vp9", ADVC_DIRECTION_ENCODE },
    { "video/av01", ADVC_DIRECTION_ENCODE },
};

int advc_probe_android_capabilities(struct advc_capability_set *out, char *error, size_t error_size) {
    void *media = NULL;
    get_name_fn get_name = NULL;
    release_name_fn release_name = NULL;
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->api_level = (uint32_t)android_get_device_api_level();
    /* Transport bits prove compiled public API availability, not codec-specific
     * Surface configuration or a completed frame transfer. */
    out->transport_features = ADVC_FEATURE_DECODE | ADVC_FEATURE_ENCODE |
                              ADVC_FEATURE_MEMFD;
    /* Keep QUERY_CAPABILITIES consistent with the broker HELLO feature set.
     * DECODE_PRIME remains fail-closed unless the launcher supplies the exact
     * validation token for the tested QTI PRIME/repack/fence pipeline. */
    {
        const char *validation = getenv("ADVC_DECODE_PRIME_VALIDATION");
        if (validation != NULL &&
            strcmp(validation,
                   "validated-qcom-prime-repack-linear-fence-eos-v1") == 0)
            out->transport_features |= ADVC_FEATURE_DECODE_PRIME;
    }
    if (out->api_level >= 26)
        out->transport_features |= ADVC_FEATURE_AHARDWAREBUFFER |
                                   ADVC_FEATURE_NATIVE_FENCE;
    if (out->api_level >= 26 && advc_probe_broker_egl_surface())
        out->transport_features |= ADVC_FEATURE_BROKER_EGL_SURFACE;
    if (out->api_level >= 26 && advc_probe_android_ahb_socket())
        out->transport_features |= ADVC_FEATURE_ANDROID_AHB_SURFACE;
    /* Backend bits are fail-closed and require an independent full import,
     * submit, encoded-frame, and EOS probe. The aggregate bit is set only when
     * at least one real backend passed. */
    if (out->api_level >= 26 && advc_probe_android_dmabuf_surface_backend(
            ADVC_DMABUF_SURFACE_VULKAN))
        out->transport_features |= ADVC_FEATURE_DMABUF_VULKAN |
                                   ADVC_FEATURE_DMABUF;
    if (out->api_level >= 26 && advc_probe_android_dmabuf_surface_backend(
            ADVC_DMABUF_SURFACE_EGL))
        out->transport_features |= ADVC_FEATURE_DMABUF_EGL |
                                   ADVC_FEATURE_DMABUF;

    if (out->api_level >= 28) {
        media = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
        if (media != NULL) {
            get_name = (get_name_fn)dlsym(media, "AMediaCodec_getName");
            release_name = (release_name_fn)dlsym(media, "AMediaCodec_releaseName");
        }
    }

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]) && out->count < ADVC_MAX_CAPABILITIES; ++i) {
        AMediaCodec *codec = probes[i].direction == ADVC_DIRECTION_DECODE
            ? AMediaCodec_createDecoderByType(probes[i].mime)
            : AMediaCodec_createEncoderByType(probes[i].mime);
        if (codec == NULL) continue;
        struct advc_codec_capability *cap = &out->codecs[out->count++];
        strncpy(cap->mime, probes[i].mime, sizeof(cap->mime) - 1);
        strncpy(cap->codec_name, "selected-by-mediacodec", sizeof(cap->codec_name) - 1);
        cap->direction = probes[i].direction;
        cap->acceleration = ADVC_ACCELERATION_UNKNOWN;
        if (get_name != NULL && release_name != NULL) {
            char *name = NULL;
            if (get_name(codec, &name) == AMEDIA_OK && name != NULL) {
                strncpy(cap->codec_name, name, sizeof(cap->codec_name) - 1);
                cap->acceleration = advc_classify_codec_component(name);
                release_name(codec, name);
            }
        }
        if (getenv("ADVC_DEBUG") != NULL)
            fprintf(stderr,
                    "advc-capability: selected mime=%s direction=%u "
                    "component=%s acceleration=%u decision=%s\n",
                    cap->mime, cap->direction, cap->codec_name,
                    cap->acceleration,
                    cap->acceleration == ADVC_ACCELERATION_HARDWARE
                        ? "hardware-candidate"
                        : cap->acceleration == ADVC_ACCELERATION_SOFTWARE
                              ? "reject-software"
                              : "reject-unclassified-or-secure");
        AMediaCodec_delete(codec);
    }
    if (media != NULL) dlclose(media);
    if (out->count == 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "libmediandk returned no known video codecs");
        return -1;
    }
    return 0;
}
