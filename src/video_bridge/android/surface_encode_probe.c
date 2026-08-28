#include "surface_encode_probe.h"

#include "encode_surface.h"
#include "encode_surface_egl.h"

#include <android/api-level.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void probe_debug(int enabled, const char *format, ...) {
    va_list args;
    if (!enabled) return;
    fputs("advc-surface-probe: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static int drain_encoded_eos(AMediaCodec *codec, int debug) {
    int saw_frame_bytes = 0;
    int saw_eos = 0;
    unsigned int attempts;
    for (attempts = 0; attempts < 100 && !saw_eos; ++attempts) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (attempts == 0 || (attempts + 1) % 25 == 0)
                probe_debug(debug, "drain wait attempt=%u", attempts + 1);
            continue;
        }
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            probe_debug(debug, "drain event=%zd attempt=%u", index, attempts + 1);
            continue;
        }
        if (index < 0 || info.offset < 0 || info.size < 0) {
            probe_debug(debug, "drain invalid index=%zd offset=%d size=%d flags=0x%x",
                        index, info.offset, info.size, info.flags);
            return 0;
        }
        probe_debug(debug, "drain buffer index=%zd offset=%d size=%d flags=0x%x pts_us=%lld",
                    index, info.offset, info.size, info.flags,
                    (long long)info.presentationTimeUs);
        if (info.size != 0) {
            size_t capacity = 0;
            uint8_t *bytes = AMediaCodec_getOutputBuffer(codec, (size_t)index,
                                                         &capacity);
            uint64_t offset = (uint64_t)info.offset;
            uint64_t size = (uint64_t)info.size;
            if (bytes == NULL || offset > capacity || size > capacity - offset) {
                probe_debug(debug, "drain bounds failure capacity=%zu", capacity);
                (void)AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
                return 0;
            }
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0)
                saw_frame_bytes = 1;
        }
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0)
            saw_eos = 1;
        if (AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false) !=
            AMEDIA_OK) {
            probe_debug(debug, "drain release failure index=%zd", index);
            return 0;
        }
    }
    probe_debug(debug, "drain done attempts=%u frame_bytes=%d eos=%d",
                attempts, saw_frame_bytes, saw_eos);
    return saw_frame_bytes && saw_eos;
}

int advc_probe_broker_egl_surface(void) {
    static int cached = -1;
    static const int32_t color_format_surface = (int32_t)0x7f000789u;
    static const int32_t probe_width = 320;
    static const int32_t probe_height = 240;
    static const int32_t probe_bitrate = 500000;
    static const float probe_framerate = 30.0f;
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    struct advc_encode_surface *surface = NULL;
    struct advc_egl_surface_producer *producer = NULL;
    void *window = NULL;
    int started = 0;
    int ok = 0;
    int debug = getenv("ADVC_DEBUG") != NULL;
    media_status_t media_status;
    int surface_status;
    if (cached >= 0) return cached;
    if (android_get_device_api_level() < 36) {
        probe_debug(debug, "reject runtime_api=%d required=36",
                    android_get_device_api_level());
        cached = 0;
        return cached;
    }
    probe_debug(debug,
                "begin runtime_api=%d size=%dx%d bitrate=%d framerate=%.1f",
                android_get_device_api_level(), probe_width, probe_height,
                probe_bitrate, (double)probe_framerate);
    codec = AMediaCodec_createEncoderByType("video/avc");
    format = AMediaFormat_new();
    if (codec == NULL || format == NULL) {
        probe_debug(debug, "allocation failure codec=%p format=%p",
                    (void *)codec, (void *)format);
        goto done;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, probe_width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, probe_height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, probe_bitrate);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, probe_framerate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          color_format_surface);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    media_status = AMediaCodec_configure(codec, format, NULL, NULL,
                                         AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (media_status != AMEDIA_OK) {
        probe_debug(debug, "configure failure status=%d", media_status);
        goto done;
    }
    surface_status = advc_encode_surface_create(
        codec, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
        ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW,
        advc_encode_surface_ndk_ops(), NULL, &surface);
    if (surface_status != ADVC_ENCODE_SURFACE_OK) {
        probe_debug(debug, "surface create failure status=%d", surface_status);
        goto done;
    }
    surface_status = advc_encode_surface_acquire_window(surface, &window);
    if (surface_status != ADVC_ENCODE_SURFACE_OK) {
        probe_debug(debug, "window acquire failure status=%d", surface_status);
        goto done;
    }
    if (advc_egl_surface_producer_create(window, (uint32_t)probe_width,
                                         (uint32_t)probe_height,
                                         &producer) < 0) {
        probe_debug(debug, "EGL producer create failure");
        goto done;
    }
    advc_encode_surface_release_window(surface, window);
    window = NULL;
    media_status = AMediaCodec_start(codec);
    if (media_status != AMEDIA_OK) {
        probe_debug(debug, "codec start failure status=%d", media_status);
        goto done;
    }
    started = 1;
    surface_status = advc_encode_surface_mark_started(surface);
    if (surface_status != ADVC_ENCODE_SURFACE_OK) {
        probe_debug(debug, "surface start transition failure status=%d",
                    surface_status);
        goto done;
    }
    if (advc_egl_surface_producer_render(producer, 0, 0) < 0) {
        probe_debug(debug, "EGL render failure");
        goto done;
    }
    surface_status = advc_encode_surface_signal_eos(surface);
    if (surface_status != ADVC_ENCODE_SURFACE_OK) {
        probe_debug(debug, "surface EOS failure status=%d", surface_status);
        goto done;
    }
    if (!drain_encoded_eos(codec, debug)) {
        probe_debug(debug, "encoded drain contract failure");
        goto done;
    }
    ok = 1;

done:
    if (window != NULL && surface != NULL)
        advc_encode_surface_release_window(surface, window);
    if (started) (void)AMediaCodec_stop(codec);
    advc_egl_surface_producer_destroy(producer);
    advc_encode_surface_destroy(surface);
    if (format != NULL) AMediaFormat_delete(format);
    if (codec != NULL) AMediaCodec_delete(codec);
    probe_debug(debug, "result=%s", ok ? "supported" : "unsupported");
    cached = ok;
    return cached;
}
