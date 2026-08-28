#include "encode_surface.h"

#if !defined(__ANDROID__)
#error "encode_surface_ndk.c is Android-only"
#endif

#include <android/api-level.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

static int ndk_get_api_level(void *userdata) {
    (void)userdata;
    return android_get_device_api_level();
}

static int ndk_create_input_surface(void *codec, void **window, void *userdata) {
    ANativeWindow *native_window = NULL;
    media_status_t status;
    (void)userdata;
    status = AMediaCodec_createInputSurface((AMediaCodec *)codec, &native_window);
    if (status != AMEDIA_OK || native_window == NULL) return -1;
    *window = native_window;
    return 0;
}

static void ndk_acquire_window(void *window, void *userdata) {
    (void)userdata;
    ANativeWindow_acquire((ANativeWindow *)window);
}

static void ndk_release_window(void *window, void *userdata) {
    (void)userdata;
    ANativeWindow_release((ANativeWindow *)window);
}

static int ndk_signal_eos(void *codec, void *userdata) {
    (void)userdata;
    return AMediaCodec_signalEndOfInputStream((AMediaCodec *)codec) == AMEDIA_OK
               ? 0
               : -1;
}

const struct advc_encode_surface_ops *advc_encode_surface_ndk_ops(void) {
    static const struct advc_encode_surface_ops ops = {
        .features = ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW |
                    ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY,
        .get_api_level = ndk_get_api_level,
        .create_input_surface = ndk_create_input_surface,
        .acquire_window = ndk_acquire_window,
        .release_window = ndk_release_window,
        .signal_end_of_input_stream = ndk_signal_eos,
        .submit_ahb = NULL,
        .close_fence = NULL,
    };
    return &ops;
}
