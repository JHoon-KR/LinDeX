#ifndef ADVC_TRANSPORT_POLICY_H
#define ADVC_TRANSPORT_POLICY_H

#include "advc/protocol.h"

#include <stdint.h>

enum advc_zero_copy_mode {
    ADVC_ZERO_COPY_STRICT = 1,
    ADVC_ZERO_COPY_AUTO = 2,
};

enum advc_dmabuf_backend_preference {
    ADVC_DMABUF_BACKEND_AUTO = 0,
    ADVC_DMABUF_BACKEND_VULKAN = 1,
    ADVC_DMABUF_BACKEND_EGL = 2,
};

enum advc_selected_backend {
    ADVC_BACKEND_UNAVAILABLE = 0,
    ADVC_BACKEND_VULKAN_DMABUF = 1,
    ADVC_BACKEND_EGL_DMABUF = 2,
    ADVC_BACKEND_ANDROID_AHB = 3,
    ADVC_BACKEND_BROKER_SURFACE = 4,
    ADVC_BACKEND_BYTES = 5,
};

#define ADVC_BACKEND_MASK(backend) (UINT32_C(1) << (backend))

enum advc_backend_selection_reason {
    ADVC_SELECTION_REAL_PROBE_PASS = 1,
    ADVC_SELECTION_AUTO_ANDROID_LOCAL = 2,
    ADVC_SELECTION_AUTO_BYTE_COPY = 3,
    ADVC_SELECTION_STRICT_ZERO_COPY_UNAVAILABLE = 4,
    ADVC_SELECTION_CANDIDATES_EXHAUSTED = 5,
};

struct advc_transport_policy_request {
    uint32_t zero_copy_mode;
    uint32_t dmabuf_preference;
    uint64_t available_features;
    uint32_t rejected_backends;
    uint32_t frames_submitted_in_previous_session;
    int previous_session_closed;
};

struct advc_transport_policy_result {
    uint32_t backend;
    uint32_t reason;
    uint32_t transport;
    int cpu_pixel_copy;
};

/*
 * Chooses one backend only. If it later rejects an exact descriptor, the caller
 * must close that session with zero submitted frames, add it to rejected_backends,
 * and call again. Mid-session and mid-frame switching return EBUSY.
 */
int advc_transport_policy_select(
    const struct advc_transport_policy_request *request,
    struct advc_transport_policy_result *result);
const char *advc_selected_backend_name(uint32_t backend);
const char *advc_selection_reason_name(uint32_t reason);

#endif
