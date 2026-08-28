#include "advc/transport_policy.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct advc_transport_policy_request request_with(uint64_t features) {
    struct advc_transport_policy_request request;
    memset(&request, 0, sizeof(request));
    request.zero_copy_mode = ADVC_ZERO_COPY_AUTO;
    request.dmabuf_preference = ADVC_DMABUF_BACKEND_AUTO;
    request.available_features = features;
    return request;
}

static void test_default_prefers_vulkan_then_egl(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_DMABUF | ADVC_FEATURE_DMABUF_EGL |
        ADVC_FEATURE_DMABUF_VULKAN);
    struct advc_transport_policy_result result;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_VULKAN_DMABUF);
    assert(result.transport == ADVC_TRANSPORT_DMABUF);
    assert(result.cpu_pixel_copy == 0);

    request.rejected_backends = ADVC_BACKEND_MASK(ADVC_BACKEND_VULKAN_DMABUF);
    request.previous_session_closed = 1;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_EGL_DMABUF);
}

static void test_forced_backend_does_not_silently_switch(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_DMABUF | ADVC_FEATURE_DMABUF_EGL |
        ADVC_FEATURE_DMABUF_VULKAN | ADVC_FEATURE_MEMFD);
    struct advc_transport_policy_result result;
    request.dmabuf_preference = ADVC_DMABUF_BACKEND_VULKAN;
    request.rejected_backends = ADVC_BACKEND_MASK(ADVC_BACKEND_VULKAN_DMABUF);
    request.previous_session_closed = 1;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_BYTES);
    assert(result.cpu_pixel_copy == 1);
}

static void test_strict_never_uses_copy_fallback(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_MEMFD | ADVC_FEATURE_ANDROID_AHB_SURFACE |
        ADVC_FEATURE_BROKER_EGL_SURFACE);
    struct advc_transport_policy_result result;
    request.zero_copy_mode = ADVC_ZERO_COPY_STRICT;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_UNAVAILABLE);
    assert(result.reason == ADVC_SELECTION_STRICT_ZERO_COPY_UNAVAILABLE);
    assert(result.cpu_pixel_copy == 0);
}

static void test_auto_fallback_order(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_ANDROID_AHB_SURFACE | ADVC_FEATURE_BROKER_EGL_SURFACE |
        ADVC_FEATURE_MEMFD);
    struct advc_transport_policy_result result;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_ANDROID_AHB);
    request.available_features &= ~ADVC_FEATURE_ANDROID_AHB_SURFACE;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_BROKER_SURFACE);
    request.available_features &= ~ADVC_FEATURE_BROKER_EGL_SURFACE;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_BYTES);
    assert(result.reason == ADVC_SELECTION_AUTO_BYTE_COPY);
}

static void test_retry_requires_closed_empty_session(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_DMABUF_EGL | ADVC_FEATURE_DMABUF_VULKAN);
    struct advc_transport_policy_result result;
    request.rejected_backends = ADVC_BACKEND_MASK(ADVC_BACKEND_VULKAN_DMABUF);
    errno = 0;
    assert(advc_transport_policy_select(&request, &result) < 0);
    assert(errno == EBUSY);
    request.previous_session_closed = 1;
    request.frames_submitted_in_previous_session = 1;
    errno = 0;
    assert(advc_transport_policy_select(&request, &result) < 0);
    assert(errno == EBUSY);
}

static void test_backend_specific_probe_bit_is_required(void) {
    struct advc_transport_policy_request request = request_with(
        ADVC_FEATURE_DMABUF);
    struct advc_transport_policy_result result;
    request.zero_copy_mode = ADVC_ZERO_COPY_STRICT;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_UNAVAILABLE);
    request.available_features = ADVC_FEATURE_DMABUF_VULKAN;
    assert(advc_transport_policy_select(&request, &result) == 0);
    assert(result.backend == ADVC_BACKEND_UNAVAILABLE);
}

int main(void) {
    test_default_prefers_vulkan_then_egl();
    test_forced_backend_does_not_silently_switch();
    test_strict_never_uses_copy_fallback();
    test_auto_fallback_order();
    test_retry_requires_closed_empty_session();
    test_backend_specific_probe_bit_is_required();
    assert(strcmp(advc_selected_backend_name(ADVC_BACKEND_VULKAN_DMABUF),
                  "vulkan-dmabuf") == 0);
    assert(strcmp(advc_selection_reason_name(
                      ADVC_SELECTION_STRICT_ZERO_COPY_UNAVAILABLE),
                  "strict-zero-copy-unavailable") == 0);
    puts("transport_policy_test: all tests passed");
    return 0;
}
