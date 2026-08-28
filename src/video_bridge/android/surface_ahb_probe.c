#include "surface_encode_probe.h"

#include "encode_surface.h"
#include "encode_surface_egl.h"

#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int drain_frame_and_eos(AMediaCodec *codec) {
    int frame = 0;
    int eos = 0;
    for (unsigned int attempt = 0; attempt < 100 && !eos; ++attempt) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
            index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (index < 0 || info.offset < 0 || info.size < 0) return 0;
        if (info.size > 0) {
            size_t capacity = 0;
            uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)index,
                                                        &capacity);
            uint64_t offset = (uint64_t)info.offset;
            uint64_t size = (uint64_t)info.size;
            if (data == NULL || offset > capacity || size > capacity - offset) {
                (void)AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
                return 0;
            }
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0)
                frame = 1;
        }
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) eos = 1;
        if (AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false) != AMEDIA_OK)
            return 0;
    }
    return frame && eos;
}

int advc_probe_android_ahb_surface(void) {
    static int cached = -1;
    static const uint32_t width = 320;
    static const uint32_t height = 240;
    static const int32_t color_format_surface = (int32_t)0x7f000789u;
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    AHardwareBuffer *buffer = NULL;
    AHardwareBuffer_Desc desc;
    struct advc_encode_surface *surface = NULL;
    struct advc_egl_surface_producer *producer = NULL;
    void *window = NULL;
    void *pixels = NULL;
    int acquire_fence = -1;
    int fence_to_render = -1;
    int release_fence = -1;
    int started = 0;
    int locked = 0;
    int ok = 0;
    if (cached >= 0) return cached;
    if (android_get_device_api_level() < 36) goto done;
    memset(&desc, 0, sizeof(desc));
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0 || buffer == NULL ||
        AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1,
                             NULL, &pixels) != 0 || pixels == NULL)
        goto done;
    locked = 1;
    AHardwareBuffer_describe(buffer, &desc);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)pixels + (size_t)y * desc.stride;
        for (uint32_t x = 0; x < width; ++x)
            row[x] = UINT32_C(0xff3366cc);
    }
    if (AHardwareBuffer_unlock(buffer, &acquire_fence) != 0) goto done;
    locked = 0;
    codec = AMediaCodec_createEncoderByType("video/avc");
    format = AMediaFormat_new();
    if (codec == NULL || format == NULL) goto done;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, (int32_t)width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, 500000);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, 30.0f);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          color_format_surface);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    if (AMediaCodec_configure(codec, format, NULL, NULL,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        advc_encode_surface_create(codec, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
            ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, advc_encode_surface_ndk_ops(),
            NULL, &surface) != ADVC_ENCODE_SURFACE_OK ||
        advc_encode_surface_acquire_window(surface, &window) !=
            ADVC_ENCODE_SURFACE_OK ||
        advc_egl_surface_producer_create(window, width, height, &producer) < 0)
        goto done;
    advc_encode_surface_release_window(surface, window);
    window = NULL;
    if (AMediaCodec_start(codec) != AMEDIA_OK) goto done;
    started = 1;
    if (advc_encode_surface_mark_started(surface) != ADVC_ENCODE_SURFACE_OK)
        goto done;
    fence_to_render = acquire_fence;
    acquire_fence = -1;
    if (advc_egl_surface_producer_render_ahb(producer, buffer, 0, 0,
                                             fence_to_render,
                                             &release_fence) < 0)
        goto done;
    if (release_fence >= 0) {
        close(release_fence);
        release_fence = -1;
    }
    if (advc_encode_surface_signal_eos(surface) != ADVC_ENCODE_SURFACE_OK ||
        !drain_frame_and_eos(codec))
        goto done;
    ok = 1;
done:
    if (locked && buffer != NULL) (void)AHardwareBuffer_unlock(buffer, NULL);
    if (acquire_fence >= 0) close(acquire_fence);
    if (release_fence >= 0) close(release_fence);
    if (window != NULL && surface != NULL)
        advc_encode_surface_release_window(surface, window);
    if (started) (void)AMediaCodec_stop(codec);
    advc_egl_surface_producer_destroy(producer);
    advc_encode_surface_destroy(surface);
    if (format != NULL) AMediaFormat_delete(format);
    if (codec != NULL) AMediaCodec_delete(codec);
    if (buffer != NULL) AHardwareBuffer_release(buffer);
    cached = ok;
    return cached;
}
