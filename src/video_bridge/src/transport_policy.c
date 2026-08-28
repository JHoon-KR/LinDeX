#include "advc/transport_policy.h"

#include <errno.h>
#include <string.h>

static int available(const struct advc_transport_policy_request *request,
                     uint32_t backend, uint64_t feature) {
    const int dmabuf_backend = backend == ADVC_BACKEND_VULKAN_DMABUF ||
                               backend == ADVC_BACKEND_EGL_DMABUF;
    return (!dmabuf_backend ||
            (request->available_features & ADVC_FEATURE_DMABUF) != 0) &&
           (request->available_features & feature) != 0 &&
           (request->rejected_backends & ADVC_BACKEND_MASK(backend)) == 0;
}

static void choose(struct advc_transport_policy_result *result,
                   uint32_t backend, uint32_t reason, uint32_t transport,
                   int cpu_pixel_copy) {
    result->backend = backend;
    result->reason = reason;
    result->transport = transport;
    result->cpu_pixel_copy = cpu_pixel_copy;
}

int advc_transport_policy_select(
    const struct advc_transport_policy_request *request,
    struct advc_transport_policy_result *result) {
    uint32_t first;
    uint32_t second;
    uint64_t first_feature;
    uint64_t second_feature;
    if (request == NULL || result == NULL ||
        (request->zero_copy_mode != ADVC_ZERO_COPY_STRICT &&
         request->zero_copy_mode != ADVC_ZERO_COPY_AUTO) ||
        request->dmabuf_preference > ADVC_DMABUF_BACKEND_EGL) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (request->rejected_backends != 0 &&
        (request->frames_submitted_in_previous_session != 0 ||
         !request->previous_session_closed)) {
        errno = EBUSY;
        return -1;
    }
    if (request->dmabuf_preference == ADVC_DMABUF_BACKEND_EGL) {
        first = ADVC_BACKEND_EGL_DMABUF;
        first_feature = ADVC_FEATURE_DMABUF_EGL;
        second = ADVC_BACKEND_VULKAN_DMABUF;
        second_feature = ADVC_FEATURE_DMABUF_VULKAN;
    } else {
        first = ADVC_BACKEND_VULKAN_DMABUF;
        first_feature = ADVC_FEATURE_DMABUF_VULKAN;
        second = ADVC_BACKEND_EGL_DMABUF;
        second_feature = ADVC_FEATURE_DMABUF_EGL;
    }
    if (available(request, first, first_feature)) {
        choose(result, first, ADVC_SELECTION_REAL_PROBE_PASS,
               ADVC_TRANSPORT_DMABUF, 0);
        return 0;
    }
    if (request->dmabuf_preference == ADVC_DMABUF_BACKEND_AUTO &&
        available(request, second, second_feature)) {
        choose(result, second, ADVC_SELECTION_REAL_PROBE_PASS,
               ADVC_TRANSPORT_DMABUF, 0);
        return 0;
    }
    if (request->zero_copy_mode == ADVC_ZERO_COPY_STRICT) {
        choose(result, ADVC_BACKEND_UNAVAILABLE,
               ADVC_SELECTION_STRICT_ZERO_COPY_UNAVAILABLE, 0, 0);
        return 0;
    }
    if (available(request, ADVC_BACKEND_ANDROID_AHB,
                  ADVC_FEATURE_ANDROID_AHB_SURFACE)) {
        choose(result, ADVC_BACKEND_ANDROID_AHB,
               ADVC_SELECTION_AUTO_ANDROID_LOCAL,
               ADVC_TRANSPORT_ANDROID_AHB_SURFACE, 0);
        return 0;
    }
    if (available(request, ADVC_BACKEND_BROKER_SURFACE,
                  ADVC_FEATURE_BROKER_EGL_SURFACE)) {
        choose(result, ADVC_BACKEND_BROKER_SURFACE,
               ADVC_SELECTION_AUTO_ANDROID_LOCAL,
               ADVC_TRANSPORT_BROKER_EGL_SURFACE, 0);
        return 0;
    }
    if (available(request, ADVC_BACKEND_BYTES, ADVC_FEATURE_MEMFD)) {
        choose(result, ADVC_BACKEND_BYTES,
               ADVC_SELECTION_AUTO_BYTE_COPY, ADVC_TRANSPORT_BYTES, 1);
        return 0;
    }
    choose(result, ADVC_BACKEND_UNAVAILABLE,
           ADVC_SELECTION_CANDIDATES_EXHAUSTED, 0, 0);
    return 0;
}

const char *advc_selected_backend_name(uint32_t backend) {
    switch (backend) {
    case ADVC_BACKEND_VULKAN_DMABUF: return "vulkan-dmabuf";
    case ADVC_BACKEND_EGL_DMABUF: return "egl-dmabuf";
    case ADVC_BACKEND_ANDROID_AHB: return "android-ahb-surface";
    case ADVC_BACKEND_BROKER_SURFACE: return "broker-egl-surface";
    case ADVC_BACKEND_BYTES: return "byte-copy";
    default: return "unavailable";
    }
}

const char *advc_selection_reason_name(uint32_t reason) {
    switch (reason) {
    case ADVC_SELECTION_REAL_PROBE_PASS: return "real-probe-pass";
    case ADVC_SELECTION_AUTO_ANDROID_LOCAL: return "auto-android-local";
    case ADVC_SELECTION_AUTO_BYTE_COPY: return "auto-byte-copy";
    case ADVC_SELECTION_STRICT_ZERO_COPY_UNAVAILABLE:
        return "strict-zero-copy-unavailable";
    default: return "candidates-exhausted";
    }
}
