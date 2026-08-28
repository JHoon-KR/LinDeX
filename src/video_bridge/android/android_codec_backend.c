#include "android_codec_backend.h"
#include "android_ahb_decode.h"
#include "encode_surface.h"
#include "encode_surface_egl.h"
#include "encode_surface_vulkan.h"
#include "surface_encode_probe.h"
#if defined(__ANDROID__)
#include "ahb_transport.h"
#include "android_prime_mapper.h"
#endif
#include "advc/byte_range.h"

#include <android/api-level.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

struct android_codec_session {
    AMediaCodec *codec;
    struct advc_ahb_surface *ahb_surface;
    struct advc_encode_surface *encode_surface;
    struct advc_egl_surface_producer *egl_producer;
    struct advc_vk_surface_producer *vk_producer;
#if defined(__ANDROID__)
    struct advc_ahb_prime_mapper *prime_mapper;
#endif
    void *native_window;
    ssize_t pending_input_index;
    uint32_t width;
    uint32_t height;
    uint32_t android_format;
    uint32_t stride;
    uint32_t slice_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_right;
    uint32_t crop_bottom;
    uint32_t api_level;
    uint32_t dequeue_calls;
    uint32_t bitrate;
    uint32_t framerate_milli;
    uint32_t color_format;
    uint32_t transport;
    uint32_t encode_profile;
    uint64_t local_surface_frames;
    uint64_t local_surface_last_pts_ns;
    int local_surface_have_pts;
    uint32_t local_surface_pending;
    int surface_transport_failed;
    uint8_t *pending_codec_config;
    size_t pending_codec_config_size;
    uint8_t direction;
    char mime[ADVC_MAX_MIME];
    int encoder_csd_emitted;
    int input_eos_queued;
    int output_eos_seen;
    int pending_surface_output;
    AMediaCodecBufferInfo pending_surface_info;
    int started;
    int debug;
};

/* These MediaFormat keys are stable in the Java API but absent from NDK r27d headers. */
static const char key_crop_left[] = "crop-left";
static const char key_crop_top[] = "crop-top";
static const char key_crop_right[] = "crop-right";
static const char key_crop_bottom[] = "crop-bottom";
static const char key_profile[] = "profile";
static const char key_latency[] = "latency";
static const char key_low_latency[] = "low-latency";
static const char key_max_b_frames[] = "max-bframes";
static const char key_priority[] = "priority";
static const char key_bitrate_mode[] = "bitrate-mode";
static const char key_operating_rate[] = "operating-rate";
static const int32_t color_format_yuv420_flexible = (int32_t)0x7f420888u;
static const int32_t android_avc_profile_constrained_baseline =
    (int32_t)0x10000u;
static const int32_t android_avc_profile_main = (int32_t)0x02u;
static const int32_t android_avc_profile_high = (int32_t)0x08u;
static const int32_t android_hevc_profile_main = (int32_t)0x01u;
/*
 * One broker DEQUEUE_OUTPUT request may wait briefly for the Codec2 callback
 * bridge, but it must never inherit the client's multi-second deadline.
 */
static const int64_t output_dequeue_timeout_us = 10000;
static const uint32_t max_output_event_spins = 8;
/* Control-only tokens can never alias a real codec index or aligned AImage. */
static const uintptr_t output_token_codec_config = UINTPTR_MAX;
static const uintptr_t output_token_surface_eos = UINTPTR_MAX - 1u;

/*
 * The v1 encode contract is VBR-only, so make the Android MediaCodec rate
 * control explicit instead of relying on a vendor component's unspecified
 * default. Exact diagnostic overrides remain available for isolated A/B
 * testing only. Android documents low-latency primarily for decoders, so
 * encoder acceptance is not evidence that lookahead was actually removed.
 */
static void apply_encode_rate_control_and_diagnostics(AMediaFormat *format) {
    const char *low_latency = getenv("ADVC_CODEC_DIAGNOSTIC_LOW_LATENCY");
    const char *bitrate_mode = getenv("ADVC_CODEC_DIAGNOSTIC_BITRATE_MODE");
    const char *operating_rate = getenv("ADVC_CODEC_DIAGNOSTIC_OPERATING_RATE");

    if (low_latency != NULL &&
        strcmp(low_latency, "validated-0-v1") == 0)
        AMediaFormat_setInt32(format, key_low_latency, 0);
    else if (low_latency != NULL &&
             strcmp(low_latency, "validated-1-v1") == 0)
        AMediaFormat_setInt32(format, key_low_latency, 1);

    if (bitrate_mode != NULL &&
             strcmp(bitrate_mode, "validated-cbr-v1") == 0)
        AMediaFormat_setInt32(format, key_bitrate_mode, 2);
    else
        /* MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR. */
        AMediaFormat_setInt32(format, key_bitrate_mode, 1);

    if (operating_rate != NULL &&
        strcmp(operating_rate, "validated-30-v1") == 0)
        AMediaFormat_setFloat(format, key_operating_rate, 30.0f);
    else if (operating_rate != NULL &&
             strcmp(operating_rate, "validated-120-v1") == 0)
        AMediaFormat_setFloat(format, key_operating_rate, 120.0f);
}

static void apply_decode_latency_diagnostics(AMediaFormat *format) {
    const char *mode = getenv("ADVC_CODEC_DECODE_LOW_LATENCY");
    if (mode == NULL || strcmp(mode, "firefox-rdd-v1") != 0) return;
    AMediaFormat_setInt32(format, key_low_latency, 1);
    AMediaFormat_setInt32(format, key_priority, 0);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60.0f);
    AMediaFormat_setFloat(format, key_operating_rate, 240.0f);
}

static uint32_t media_status_to_advc(media_status_t status) {
    if (status == AMEDIA_OK) return ADVC_STATUS_OK;
    if (status == AMEDIA_ERROR_UNSUPPORTED) return ADVC_STATUS_UNSUPPORTED;
    if (status == AMEDIA_ERROR_INVALID_PARAMETER) return ADVC_STATUS_BAD_MESSAGE;
    return ADVC_STATUS_CODEC_ERROR;
}

static int valid_encoder_config(const struct advc_backend_config *config) {
    uint64_t pixels;
    uint64_t bytes;
    uint32_t transport = config->transport == 0 ? ADVC_TRANSPORT_BYTES :
                                                  config->transport;
    if (config->direction != ADVC_DIRECTION_ENCODE ||
        (strcmp(config->mime, "video/avc") != 0 &&
         strcmp(config->mime, "video/hevc") != 0) ||
        config->width < 16 || config->width > 8192 ||
        config->height < 16 || config->height > 8192 ||
        (config->width & 1u) != 0 || (config->height & 1u) != 0 ||
        config->bitrate == 0 || config->bitrate > ADVC_MAX_ENCODE_BITRATE ||
        config->framerate_milli < 1000 || config->framerate_milli > 240000 ||
        ((strcmp(config->mime, "video/avc") == 0 &&
          config->encode_profile !=
              ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE &&
          config->encode_profile != ADVC_ENCODE_PROFILE_H264_MAIN &&
          config->encode_profile != ADVC_ENCODE_PROFILE_H264_HIGH) ||
         (strcmp(config->mime, "video/hevc") == 0 &&
          config->encode_profile != ADVC_ENCODE_PROFILE_HEVC_MAIN) ||
         (transport == ADVC_TRANSPORT_BYTES &&
          config->color_format != ADVC_COLOR_FORMAT_YUV420_PLANAR &&
          config->color_format != ADVC_COLOR_FORMAT_YUV420_SEMIPLANAR) ||
         ((transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
           transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
           transport == ADVC_TRANSPORT_DMABUF) &&
          config->color_format != 0) ||
         (transport != ADVC_TRANSPORT_BYTES &&
          transport != ADVC_TRANSPORT_BROKER_EGL_SURFACE &&
          transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE &&
          transport != ADVC_TRANSPORT_DMABUF)))
        return 0;
    if (transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
        transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
        transport == ADVC_TRANSPORT_DMABUF) return 1;
    pixels = (uint64_t)config->width * (uint64_t)config->height;
    bytes = pixels + pixels / 2u;
    return bytes <= ADVC_MAX_INPUT_BYTES;
}

static int android_encode_profile(uint32_t profile, int32_t *value) {
    if (value == NULL) return -1;
    switch (profile) {
    case ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE:
        *value = android_avc_profile_constrained_baseline;
        return 0;
    case ADVC_ENCODE_PROFILE_H264_MAIN:
        *value = android_avc_profile_main;
        return 0;
    case ADVC_ENCODE_PROFILE_H264_HIGH:
        *value = android_avc_profile_high;
        return 0;
    case ADVC_ENCODE_PROFILE_HEVC_MAIN:
        *value = android_hevc_profile_main;
        return 0;
    default:
        return -1;
    }
}

static int find_annexb_start(const uint8_t *data, size_t size, size_t from,
                             size_t *offset, size_t *length) {
    for (size_t i = from; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *length = 3;
            return 0;
        }
        if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
            *offset = i;
            *length = 4;
            return 0;
        }
    }
    return -1;
}

static int set_avc_csd(AMediaFormat *format, const uint8_t *csd, size_t csd_size) {
    size_t first;
    size_t first_length;
    size_t second;
    size_t second_length;
    if (find_annexb_start(csd, csd_size, 0, &first, &first_length) < 0 || first != 0 ||
        first + first_length >= csd_size ||
        (csd[first + first_length] & 0x1fu) != 7 ||
        find_annexb_start(csd, csd_size, first + first_length + 1, &second,
                          &second_length) < 0 ||
        second + second_length >= csd_size ||
        (csd[second + second_length] & 0x1fu) != 8)
        return -1;
    AMediaFormat_setBuffer(format, "csd-0", (void *)csd, second);
    AMediaFormat_setBuffer(format, "csd-1", (void *)(csd + second), csd_size - second);
    return 0;
}

static uint32_t start_codec(struct android_codec_session *session,
                            const uint8_t *csd, size_t csd_size) {
    AMediaFormat *format = AMediaFormat_new();
    media_status_t status;
    if (format == NULL) return ADVC_STATUS_NO_RESOURCE;
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, session->mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, (int32_t)session->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)session->height);
    if (session->framerate_milli > 0)
        AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE,
                              (float)session->framerate_milli / 1000.0f);
    if (session->direction == ADVC_DIRECTION_ENCODE) {
        int32_t profile;
        if (android_encode_profile(session->encode_profile, &profile) < 0) {
            AMediaFormat_delete(format);
            return ADVC_STATUS_UNSUPPORTED;
        }
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                              (int32_t)session->bitrate);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                              (int32_t)session->color_format);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32(format, key_profile, profile);
        apply_encode_rate_control_and_diagnostics(format);
    } else {
        apply_decode_latency_diagnostics(format);
        if (getenv("ADVC_COLOR_YUV420_FLEXIBLE") != NULL)
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                                  color_format_yuv420_flexible);
    }
    if (csd != NULL && csd_size > 0 &&
        (strcmp(session->mime, "video/avc") != 0 ||
         set_avc_csd(format, csd, csd_size) < 0))
        AMediaFormat_setBuffer(format, "csd-0", (void *)csd, csd_size);
    status = AMediaCodec_configure(
        session->codec, format, session->native_window, NULL,
        session->direction == ADVC_DIRECTION_ENCODE ?
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0);
    AMediaFormat_delete(format);
    if (status != AMEDIA_OK) return media_status_to_advc(status);
    status = AMediaCodec_start(session->codec);
    if (status != AMEDIA_OK) return media_status_to_advc(status);
    session->started = 1;
    if (session->debug)
        fprintf(stderr, "advc-codec: started csd=%zu\n", csd_size);
    return ADVC_STATUS_OK;
}

static uint32_t start_surface_encoder(struct android_codec_session *session) {
    static const int32_t color_format_surface = (int32_t)0x7f000789u;
    AMediaFormat *format = AMediaFormat_new();
    void *window = NULL;
    media_status_t media_status;
    int surface_status;
    int32_t profile;
    if (format == NULL) return ADVC_STATUS_NO_RESOURCE;
    if (android_encode_profile(session->encode_profile, &profile) < 0) {
        AMediaFormat_delete(format);
        return ADVC_STATUS_UNSUPPORTED;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, session->mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, (int32_t)session->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)session->height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE,
                          (int32_t)session->bitrate);
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE,
                          (float)session->framerate_milli / 1000.0f);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          color_format_surface);
    AMediaFormat_setInt32(
        format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
        getenv("ADVC_SURFACE_ALL_INTRA") != NULL &&
                strcmp(getenv("ADVC_SURFACE_ALL_INTRA"),
                       "validated-v1") == 0
            ? 0
            : 1);
    AMediaFormat_setInt32(format, key_profile, profile);
    apply_encode_rate_control_and_diagnostics(format);
    if (getenv("ADVC_SURFACE_LOW_LATENCY") != NULL &&
        strcmp(getenv("ADVC_SURFACE_LOW_LATENCY"), "validated-v1") == 0) {
        AMediaFormat_setInt32(format, key_latency, 0);
        AMediaFormat_setInt32(format, key_max_b_frames, 0);
        AMediaFormat_setInt32(format, key_priority, 0);
    }
    media_status = AMediaCodec_configure(session->codec, format, NULL, NULL,
                                         AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (media_status != AMEDIA_OK) return media_status_to_advc(media_status);
    surface_status = advc_encode_surface_create(
        session->codec, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
        ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, advc_encode_surface_ndk_ops(),
        NULL, &session->encode_surface);
    if (surface_status != ADVC_ENCODE_SURFACE_OK ||
        advc_encode_surface_acquire_window(session->encode_surface, &window) !=
            ADVC_ENCODE_SURFACE_OK ||
        ((session->transport == ADVC_TRANSPORT_DMABUF &&
          advc_android_dmabuf_surface_route() == ADVC_DMABUF_SURFACE_VULKAN)
             ? advc_vk_surface_producer_create(
                   window, session->width, session->height,
                   &session->vk_producer)
             : advc_egl_surface_producer_create(
                   window, session->width, session->height,
                   &session->egl_producer)) < 0) {
        if (window != NULL)
            advc_encode_surface_release_window(session->encode_surface, window);
        advc_encode_surface_destroy(session->encode_surface);
        session->encode_surface = NULL;
        return ADVC_STATUS_UNSUPPORTED;
    }
    advc_encode_surface_release_window(session->encode_surface, window);
    media_status = AMediaCodec_start(session->codec);
    if (media_status != AMEDIA_OK ||
        advc_encode_surface_mark_started(session->encode_surface) !=
            ADVC_ENCODE_SURFACE_OK) {
        advc_egl_surface_producer_destroy(session->egl_producer);
        session->egl_producer = NULL;
        advc_vk_surface_producer_destroy(session->vk_producer);
        session->vk_producer = NULL;
        advc_encode_surface_destroy(session->encode_surface);
        session->encode_surface = NULL;
        return media_status != AMEDIA_OK ? media_status_to_advc(media_status) :
                                          ADVC_STATUS_CODEC_ERROR;
    }
    session->started = 1;
    return ADVC_STATUS_OK;
}

static uint32_t android_create(void *userdata, const struct advc_backend_config *config,
                               void **handle) {
    struct android_codec_session *session;
    AMediaCodec *codec;
    uint32_t status;
    (void)userdata;
    if (config == NULL || handle == NULL) return ADVC_STATUS_BAD_MESSAGE;
    *handle = NULL;
#if defined(__ANDROID__)
    if (config->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE &&
        !advc_probe_android_ahb_surface())
        return ADVC_STATUS_UNSUPPORTED;
    if (config->transport == ADVC_TRANSPORT_DMABUF &&
        !advc_probe_android_dmabuf_surface())
        return ADVC_STATUS_UNSUPPORTED;
#else
    if (config->transport == ADVC_TRANSPORT_DMABUF)
        return ADVC_STATUS_UNSUPPORTED;
#endif
    if (config->direction != ADVC_DIRECTION_DECODE &&
        config->direction != ADVC_DIRECTION_ENCODE)
        return ADVC_STATUS_UNSUPPORTED;
    if (config->direction == ADVC_DIRECTION_ENCODE && !valid_encoder_config(config))
        return ADVC_STATUS_BAD_MESSAGE;

    {
        const char *codec_name = getenv("ADVC_CODEC_NAME");
        codec = codec_name != NULL ? AMediaCodec_createCodecByName(codec_name) :
                config->direction == ADVC_DIRECTION_ENCODE ?
                    AMediaCodec_createEncoderByType(config->mime) :
                    AMediaCodec_createDecoderByType(config->mime);
    }
    if (codec == NULL) return ADVC_STATUS_UNSUPPORTED;
    session = (struct android_codec_session *)calloc(1, sizeof(*session));
    if (session == NULL) {
        AMediaCodec_delete(codec);
        return ADVC_STATUS_NO_RESOURCE;
    }
    session->codec = codec;
    session->debug = getenv("ADVC_DEBUG") != NULL;
    session->pending_input_index = -1;
    session->width = config->width;
    session->height = config->height;
    session->direction = config->direction;
    session->bitrate = config->bitrate;
    session->framerate_milli = config->framerate_milli;
    session->color_format = config->color_format;
    session->transport = config->transport == 0 ? ADVC_TRANSPORT_BYTES :
                                                  config->transport;
    session->encode_profile = config->encode_profile;
    memcpy(session->mime, config->mime, strnlen(config->mime, sizeof(session->mime) - 1));
    if (config->direction == ADVC_DIRECTION_DECODE) {
        session->crop_right = config->width - 1;
        session->crop_bottom = config->height - 1;
    }
    {
        int api_level = android_get_device_api_level();
        session->api_level = api_level > 0 ? (uint32_t)api_level : 0;
    }
    if (session->transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        if (session->direction != ADVC_DIRECTION_DECODE || session->api_level < 26 ||
            advc_ahb_surface_create(session->width, session->height,
                                    ADVC_MAX_OUTSTANDING_OUTPUTS,
                                    &session->ahb_surface,
                                    &session->native_window) < 0) {
            AMediaCodec_delete(codec);
            free(session);
            return ADVC_STATUS_UNSUPPORTED;
        }
    }
    /*
     * Decoder codec-specific data belongs in the initial MediaFormat whenever
     * the client supplies it first. This is the normal Codec2 path. The
     * diagnostic override retains the old behavior of queueing CSD as data.
     */
    if (config->direction == ADVC_DIRECTION_ENCODE ||
        getenv("ADVC_CODEC_CONFIG_AS_DATA") != NULL) {
        status = (session->transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
                  session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
                  session->transport == ADVC_TRANSPORT_DMABUF) ?
                 start_surface_encoder(session) : start_codec(session, NULL, 0);
        if (status != ADVC_STATUS_OK) {
            advc_ahb_surface_destroy(session->ahb_surface);
            advc_egl_surface_producer_destroy(session->egl_producer);
            advc_vk_surface_producer_destroy(session->vk_producer);
            advc_encode_surface_destroy(session->encode_surface);
            AMediaCodec_delete(codec);
            free(session);
            return status;
        }
    }
    *handle = session;
    if (session->debug)
        fprintf(stderr,
                "advc-codec: created direction=%u mime=%s size=%ux%u api=%u "
                "bitrate=%u fps_milli=%u color=0x%x\n",
                config->direction, config->mime, config->width, config->height,
                session->api_level, config->bitrate, config->framerate_milli,
                config->color_format);
    return ADVC_STATUS_OK;
}

static uint32_t android_queue_input(void *userdata, void *handle, const uint8_t *data,
                                    size_t size, uint64_t pts_ns, uint32_t flags) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    uint32_t media_flags = 0;
    size_t capacity = 0;
    uint8_t *buffer;
    ssize_t index;
    media_status_t status;
    (void)userdata;
    if (session == NULL || (size > 0 && data == NULL)) return ADVC_STATUS_BAD_MESSAGE;
    if (session->input_eos_queued) return ADVC_STATUS_BAD_MESSAGE;
    if (session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
        session->transport == ADVC_TRANSPORT_DMABUF) {
        if (session->direction != ADVC_DIRECTION_ENCODE || !session->started ||
            size != 0 || flags != ADVC_FLAG_END_OF_STREAM)
            return ADVC_STATUS_BAD_MESSAGE;
        if (session->egl_producer != NULL &&
            advc_egl_surface_producer_discard_import_caches(
                session->egl_producer) < 0)
            return ADVC_STATUS_CODEC_ERROR;
        if (advc_encode_surface_signal_eos(session->encode_surface) !=
            ADVC_ENCODE_SURFACE_OK)
            return ADVC_STATUS_CODEC_ERROR;
        session->input_eos_queued = 1;
        return ADVC_STATUS_OK;
    }
    if (session->transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE) {
        if (session->direction != ADVC_DIRECTION_ENCODE || !session->started ||
            size != 0 || (flags != 0 && flags != ADVC_FLAG_END_OF_STREAM))
            return ADVC_STATUS_BAD_MESSAGE;
        if (flags == ADVC_FLAG_END_OF_STREAM) {
            if (advc_encode_surface_signal_eos(session->encode_surface) !=
                ADVC_ENCODE_SURFACE_OK)
                return ADVC_STATUS_CODEC_ERROR;
            session->input_eos_queued = 1;
            return ADVC_STATUS_OK;
        }
        if (pts_ns > (uint64_t)INT64_MAX ||
            (session->local_surface_have_pts &&
             pts_ns <= session->local_surface_last_pts_ns))
            return ADVC_STATUS_BAD_MESSAGE;
        if (session->local_surface_pending >= ADVC_MAX_INFLIGHT_DMABUFS)
            return ADVC_STATUS_WOULD_BLOCK;
        if (advc_egl_surface_producer_render(
                session->egl_producer, session->local_surface_frames,
                (int64_t)pts_ns) < 0)
            return ADVC_STATUS_CODEC_ERROR;
        ++session->local_surface_frames;
        session->local_surface_last_pts_ns = pts_ns;
        session->local_surface_have_pts = 1;
        ++session->local_surface_pending;
        return ADVC_STATUS_OK;
    }
    if (!session->started) {
        uint32_t start_status;
        if (session->direction == ADVC_DIRECTION_ENCODE)
            return ADVC_STATUS_CODEC_ERROR;
        if ((flags & ADVC_FLAG_CODEC_CONFIG) != 0) {
            if ((flags & ADVC_FLAG_END_OF_STREAM) != 0 || size == 0)
                return ADVC_STATUS_BAD_MESSAGE;
            return start_codec(session, data, size);
        }
        start_status = start_codec(session, NULL, 0);
        if (start_status != ADVC_STATUS_OK) return start_status;
    }
    if (session->pending_input_index >= 0) {
        index = session->pending_input_index;
    } else {
        index = AMediaCodec_dequeueInputBuffer(session->codec, 0);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return ADVC_STATUS_WOULD_BLOCK;
        if (index < 0) return ADVC_STATUS_CODEC_ERROR;
    }
    buffer = AMediaCodec_getInputBuffer(session->codec, (size_t)index, &capacity);
    if (buffer == NULL) {
        session->pending_input_index = -1;
        return ADVC_STATUS_CODEC_ERROR;
    }
    if (size > capacity) {
        session->pending_input_index = index;
        return ADVC_STATUS_NO_RESOURCE;
    }
    if (size > 0) memcpy(buffer, data, size);
    if ((flags & ADVC_FLAG_END_OF_STREAM) != 0)
        media_flags |= AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
    if ((flags & ADVC_FLAG_CODEC_CONFIG) != 0 &&
        getenv("ADVC_CODEC_CONFIG_AS_DATA") == NULL)
        media_flags |= AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
    /*
     * MediaCodec decoder input only needs CODEC_CONFIG and END_OF_STREAM.
     * KEY_FRAME describes the compressed access unit at the ADVC layer, but
     * Android documents it as unused by most codecs on input. Do not expose a
     * vendor decoder to that unnecessary input flag.
     */
    status = AMediaCodec_queueInputBuffer(session->codec, (size_t)index, 0, size,
                                          pts_ns / 1000u, media_flags);
    if (session->debug)
        fprintf(stderr,
                "advc-codec: queue index=%zd size=%zu flags=0x%x media=0x%x status=%d\n",
                index, size, flags, media_flags, (int)status);
    session->pending_input_index = -1;
    if (status == AMEDIA_OK && (flags & ADVC_FLAG_END_OF_STREAM) != 0)
        session->input_eos_queued = 1;
    return media_status_to_advc(status);
}

static int capture_encoder_csd(struct android_codec_session *session,
                               AMediaFormat *format) {
    void *csd0 = NULL;
    void *csd1 = NULL;
    size_t csd0_size = 0;
    size_t csd1_size = 0;
    size_t total;
    uint8_t *copy;
    if (session->direction != ADVC_DIRECTION_ENCODE ||
        session->encoder_csd_emitted ||
        session->pending_codec_config != NULL)
        return 0;
    (void)AMediaFormat_getBuffer(format, "csd-0", &csd0, &csd0_size);
    (void)AMediaFormat_getBuffer(format, "csd-1", &csd1, &csd1_size);
    if (csd0_size == 0 && csd1_size == 0) return 0;
    if ((csd0_size > 0 && csd0 == NULL) || (csd1_size > 0 && csd1 == NULL) ||
        csd0_size > ADVC_MAX_OUTPUT_BYTES ||
        csd1_size > ADVC_MAX_OUTPUT_BYTES - csd0_size)
        return -1;
    total = csd0_size + csd1_size;
    copy = (uint8_t *)malloc(total);
    if (copy == NULL) return -1;
    if (csd0_size > 0) memcpy(copy, csd0, csd0_size);
    if (csd1_size > 0) memcpy(copy + csd0_size, csd1, csd1_size);
    session->pending_codec_config = copy;
    session->pending_codec_config_size = total;
    session->encoder_csd_emitted = 1;
    return 0;
}

static int update_output_format(struct android_codec_session *session) {
    AMediaFormat *format = AMediaCodec_getOutputFormat(session->codec);
    int32_t value;
    int result;
    if (format == NULL) return -1;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &value) && value > 0)
        session->width = (uint32_t)value;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &value) && value > 0)
        session->height = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE) {
        session->crop_left = 0;
        session->crop_top = 0;
        session->crop_right = session->width - 1;
        session->crop_bottom = session->height - 1;
    }
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &value) && value > 0)
        session->android_format = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &value) && value > 0)
        session->stride = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &value) && value > 0)
        session->slice_height = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, key_crop_left, &value) && value >= 0)
        session->crop_left = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, key_crop_top, &value) && value >= 0)
        session->crop_top = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, key_crop_right, &value) && value >= 0)
        session->crop_right = (uint32_t)value;
    if (session->direction == ADVC_DIRECTION_DECODE &&
        AMediaFormat_getInt32(format, key_crop_bottom, &value) && value >= 0)
        session->crop_bottom = (uint32_t)value;
    result = capture_encoder_csd(session, format);
    AMediaFormat_delete(format);
    return result;
}

static void fill_output_layout(const struct android_codec_session *session,
                               struct advc_backend_output *output) {
    output->width = session->width;
    output->height = session->height;
    output->android_format = session->android_format;
    output->stride = session->stride;
    output->slice_height = session->slice_height;
    output->crop_left = session->crop_left;
    output->crop_top = session->crop_top;
    output->crop_right = session->crop_right;
    output->crop_bottom = session->crop_bottom;
}

static void fill_output_timing_and_flags(const AMediaCodecBufferInfo *info,
                                         struct advc_backend_output *output) {
    if (info->presentationTimeUs < 0) output->pts_ns = 0;
    else if ((uint64_t)info->presentationTimeUs > UINT64_MAX / 1000u)
        output->pts_ns = UINT64_MAX;
    else output->pts_ns = (uint64_t)info->presentationTimeUs * 1000u;
    if ((info->flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0)
        output->flags |= ADVC_FLAG_END_OF_STREAM;
    if ((info->flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0)
        output->flags |= ADVC_FLAG_KEY_FRAME;
    if ((info->flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0)
        output->flags |= ADVC_FLAG_CODEC_CONFIG;
}

static uint32_t acquire_surface_output(struct android_codec_session *session,
                                       struct advc_backend_output *output) {
    void *image = NULL;
    void *buffer = NULL;
    int fence = -1;
    int64_t timestamp_ns = 0;
    uint32_t width = 0, height = 0, layers = 0, format = 0, stride = 0;
    uint64_t usage = 0;
    int result = advc_ahb_surface_acquire(session->ahb_surface, &image, &buffer,
                                          &fence, &timestamp_ns, &width, &height,
                                          &layers, &format, &stride, &usage);
    if (result == 1) return ADVC_STATUS_WOULD_BLOCK;
    if (result < 0) return ADVC_STATUS_CODEC_ERROR;
    if (width == 0 || height == 0 || layers == 0 || stride == 0) {
        advc_ahb_surface_release(image, fence);
        return ADVC_STATUS_CODEC_ERROR;
    }
    output->transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    output->native_buffer = buffer;
    output->acquire_fence_fd = fence;
    output->token = (uintptr_t)image;
    output->width = width;
    output->height = height;
    output->layers = layers;
    output->android_format = format;
    output->stride = stride;
    output->usage = usage;
    fill_output_timing_and_flags(&session->pending_surface_info, output);
    /* Image timestamp is authoritative when the codec omits a usable PTS. */
    if (output->pts_ns == 0 && timestamp_ns > 0) output->pts_ns = (uint64_t)timestamp_ns;
    output->crop_left = session->crop_left;
    output->crop_top = session->crop_top;
    output->crop_right = session->crop_right < width ? session->crop_right : width - 1;
    output->crop_bottom = session->crop_bottom < height ? session->crop_bottom : height - 1;
    session->pending_surface_output = 0;
    if ((output->flags & ADVC_FLAG_END_OF_STREAM) != 0) session->output_eos_seen = 1;
    return ADVC_STATUS_OK;
}

static uint32_t android_dequeue_output(void *userdata, void *handle,
                                       struct advc_backend_output *output) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    AMediaCodecBufferInfo info;
    size_t reported_capacity = 0;
    size_t checked_size = 0;
    const uint8_t *checked_data = NULL;
    uint8_t *buffer;
    ssize_t index;
    uint32_t event_spin;
    (void)userdata;
    if (session == NULL || output == NULL) return ADVC_STATUS_BAD_MESSAGE;
    memset(output, 0, sizeof(*output));
    output->acquire_fence_fd = -1;
    if (session->pending_surface_output)
        return acquire_surface_output(session, output);
    if (session->output_eos_seen) return ADVC_STATUS_WOULD_BLOCK;
    ++session->dequeue_calls;
    if (session->debug && session->dequeue_calls <= 3)
        fprintf(stderr, "advc-codec: dequeue enter call=%u\n", session->dequeue_calls);
    for (event_spin = 0; event_spin < max_output_event_spins; ++event_spin) {
        memset(&info, 0, sizeof(info));
        index = AMediaCodec_dequeueOutputBuffer(
            session->codec, &info,
            event_spin == 0 ? output_dequeue_timeout_us : 0);
        if (session->debug &&
            (session->dequeue_calls <= 3 ||
             index != AMEDIACODEC_INFO_TRY_AGAIN_LATER))
            fprintf(stderr,
                    "advc-codec: dequeue return call=%u event=%u index=%zd "
                    "offset=%d size=%d flags=0x%x\n",
                    session->dequeue_calls, event_spin, index, info.offset,
                    info.size, info.flags);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (update_output_format(session) < 0)
                return ADVC_STATUS_CODEC_ERROR;
            if (session->pending_codec_config != NULL) {
                output->data = session->pending_codec_config;
                output->size = session->pending_codec_config_size;
                output->flags = ADVC_FLAG_CODEC_CONFIG;
                output->token = output_token_codec_config;
                fill_output_layout(session, output);
                return ADVC_STATUS_OK;
            }
            continue;
        }
        if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        break;
    }
    if (event_spin == max_output_event_spins ||
        index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        return ADVC_STATUS_WOULD_BLOCK;
    if (index < 0) return ADVC_STATUS_CODEC_ERROR;
    if (session->transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        /*
         * A zero-sized Surface EOS has no image to acquire. Rendering it and
         * waiting on AImageReader leaves the client polling forever after the
         * final real frame. Return one empty byte-transport control output;
         * this copies no pixels and remains representable in ADVC 1.2.
         */
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0 &&
            info.size == 0) {
            if (AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                                false) != AMEDIA_OK)
                return ADVC_STATUS_CODEC_ERROR;
            output->transport = ADVC_TRANSPORT_BYTES;
            fill_output_timing_and_flags(&info, output);
            fill_output_layout(session, output);
            output->token = output_token_surface_eos;
            session->output_eos_seen = 1;
            return ADVC_STATUS_OK;
        }
        session->pending_surface_info = info;
        session->pending_surface_output = 1;
        if (AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index, true) !=
            AMEDIA_OK) {
            session->pending_surface_output = 0;
            return ADVC_STATUS_CODEC_ERROR;
        }
        return acquire_surface_output(session, output);
    }
    buffer = AMediaCodec_getOutputBuffer(session->codec, (size_t)index, &reported_capacity);
    if (session->api_level < 36) {
        AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index, false);
        return ADVC_STATUS_UNSUPPORTED;
    }
    if (advc_checked_byte_range(buffer, reported_capacity,
                                (int64_t)info.offset, (int64_t)info.size,
                                &checked_data, &checked_size) < 0) {
        AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index, false);
        return ADVC_STATUS_CODEC_ERROR;
    }
    output->data = checked_data;
    output->size = checked_size;
    fill_output_timing_and_flags(&info, output);
    fill_output_layout(session, output);
    output->token = (uintptr_t)index;
    if ((session->transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
         session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
         session->transport == ADVC_TRANSPORT_DMABUF) &&
        checked_size > 0 &&
        (output->flags & ADVC_FLAG_CODEC_CONFIG) == 0 &&
        session->local_surface_pending > 0)
        --session->local_surface_pending;
    if ((output->flags & ADVC_FLAG_END_OF_STREAM) != 0)
        session->output_eos_seen = 1;
    (void)reported_capacity;
    return ADVC_STATUS_OK;
}

static void android_release_output(void *userdata, void *handle, uintptr_t token,
                                   int release_fence_fd) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    (void)userdata;
    if (session == NULL) return;
    if (token == output_token_surface_eos) {
        if (release_fence_fd >= 0) close(release_fence_fd);
    } else if (token == output_token_codec_config) {
        if (release_fence_fd >= 0) close(release_fence_fd);
        free(session->pending_codec_config);
        session->pending_codec_config = NULL;
        session->pending_codec_config_size = 0;
    } else if (session->transport == ADVC_TRANSPORT_AHARDWAREBUFFER) {
        advc_ahb_surface_release((void *)token, release_fence_fd);
    } else {
        if (release_fence_fd >= 0) close(release_fence_fd);
        AMediaCodec_releaseOutputBuffer(session->codec, (size_t)token, false);
    }
}

static uint32_t android_flush(void *userdata, void *handle) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    (void)userdata;
    if (session == NULL) return ADVC_STATUS_BAD_MESSAGE;
    if (!session->started) return ADVC_STATUS_OK;
    /* MediaCodec input Surface EOS is one-shot; recreate the session to reuse it. */
    if (session->transport == ADVC_TRANSPORT_BROKER_EGL_SURFACE ||
        session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
        session->transport == ADVC_TRANSPORT_DMABUF) {
        if (session->egl_producer != NULL &&
            advc_egl_surface_producer_discard_import_caches(
                session->egl_producer) < 0)
            return ADVC_STATUS_CODEC_ERROR;
        return ADVC_STATUS_UNSUPPORTED;
    }
    media_status_t status = AMediaCodec_flush(session->codec);
    if (status == AMEDIA_OK) {
        if (session->ahb_surface != NULL)
            advc_ahb_surface_discard_available(session->ahb_surface);
        session->pending_input_index = -1;
        session->input_eos_queued = 0;
        session->output_eos_seen = 0;
        session->pending_surface_output = 0;
        free(session->pending_codec_config);
        session->pending_codec_config = NULL;
        session->pending_codec_config_size = 0;
        session->encoder_csd_emitted = 0;
        session->local_surface_frames = 0;
    }
    return media_status_to_advc(status);
}

static int decoder_destroy_drain_enabled(
    const struct android_codec_session *session) {
    const char *gate = getenv("ADVC_CODEC_DECODER_DESTROY_DRAIN");

    return session != NULL && session->started &&
           session->direction == ADVC_DIRECTION_DECODE && gate != NULL &&
           strcmp(gate, "validated-bounded-v1") == 0;
}

static uint32_t discard_decoder_codec_outputs_before_stop(
    struct android_codec_session *session, int drain_enabled) {
    uint32_t discarded = 0;
    uint32_t spin;

    if (!drain_enabled) return 0;

    /*
     * A VASurface-only consumer may return its VA surfaces without ever
     * asking the broker for decoded output.  In that case MediaCodec can
     * still own completed output buffers when session destruction reaches
     * AMediaCodec_stop().  Discard only outputs that are already available;
     * never wait, render, queue synthetic input or steal an AImage lease.
     * The fixed bound also makes this safe as an isolated A/B diagnostic on
     * vendor codecs whose dequeue state machine is not yet validated.
     */
    for (spin = 0;
         spin < ADVC_MAX_OUTSTANDING_OUTPUTS + max_output_event_spins;
         ++spin) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(session->codec, &info, 0);

        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
            index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
            continue;
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index < 0)
            break;
        if (AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            false) != AMEDIA_OK)
            break;
        ++discarded;
    }
    return discarded;
}

static void android_destroy(void *userdata, void *handle) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    media_status_t stop_status = AMEDIA_OK;
    uint32_t discarded_codec_outputs;
    int destroy_drain_enabled;
    (void)userdata;
    if (session == NULL) return;
    /*
     * A Surface decoder may still have rendered images queued in the
     * AImageReader even when the VA client never mapped or synchronized the
     * corresponding surfaces (for example, a VASurface-only fakesink).  Some
     * vendor codecs wait for those images while stopping.  Drain only the
     * reader-owned, currently available images before stop, then drain once
     * more after stop has cancelled production. Acquired images are owned by
     * the session engine and are released before this backend destructor is
     * called, so this cannot steal a client output lease. Keep both the reader
     * discard and codec-output discard behind the same exact experimental
     * gate: the baseline side of the live A/B must retain stock teardown.
     */
    destroy_drain_enabled = decoder_destroy_drain_enabled(session);
    if (destroy_drain_enabled && session->ahb_surface != NULL)
        advc_ahb_surface_discard_available(session->ahb_surface);
    discarded_codec_outputs =
        discard_decoder_codec_outputs_before_stop(session, destroy_drain_enabled);
    if ((session->transport == ADVC_TRANSPORT_DMABUF ||
         session->transport == ADVC_TRANSPORT_ANDROID_AHB_SURFACE) &&
        session->egl_producer != NULL)
        (void)advc_egl_surface_producer_discard_import_caches(
            session->egl_producer);
    if (session->debug)
        fprintf(stderr,
                "advc-codec: destroy before-stop direction=%u "
                "discarded-codec-outputs=%u destroy-drain=%s session=%p\n",
                session->direction, discarded_codec_outputs,
                destroy_drain_enabled ? "enabled" : "disabled", (void *)session);
    if (session->started) stop_status = AMediaCodec_stop(session->codec);
    if (session->debug)
        fprintf(stderr,
                "advc-codec: destroy after-stop status=%d direction=%u session=%p\n",
                (int)stop_status, session->direction, (void *)session);
    if (destroy_drain_enabled && session->ahb_surface != NULL)
        advc_ahb_surface_discard_available(session->ahb_surface);
    advc_egl_surface_producer_destroy(session->egl_producer);
    advc_vk_surface_producer_destroy(session->vk_producer);
    advc_encode_surface_destroy(session->encode_surface);
#if defined(__ANDROID__)
    advc_ahb_prime_mapper_destroy(session->prime_mapper);
#endif
    AMediaCodec_delete(session->codec);
    advc_ahb_surface_destroy(session->ahb_surface);
    free(session->pending_codec_config);
    free(session);
}

static int android_send_native_buffer(void *userdata, int socket_fd,
                                      void *native_buffer) {
    (void)userdata;
    return advc_ahb_send(socket_fd, native_buffer);
}

#if defined(__ANDROID__)
static uint32_t prime_errno_to_status(int error_number) {
    switch (error_number) {
    case ENOTSUP:
    case ENOSYS:
        return ADVC_STATUS_UNSUPPORTED;
    case ENOMEM:
    case ENOSPC:
        return ADVC_STATUS_NO_RESOURCE;
    case EAGAIN:
        return ADVC_STATUS_WOULD_BLOCK;
    case EINVAL:
    case EBADF:
        return ADVC_STATUS_BAD_MESSAGE;
    default:
        return ADVC_STATUS_INTERNAL;
    }
}

static uint32_t android_export_decode_prime(
    void *userdata, void *handle, void *native_buffer,
    const struct advc_ahb_public_metadata *metadata, uint64_t buffer_id,
    struct advc_dmabuf_descriptor *descriptor) {
    struct android_codec_session *session =
        (struct android_codec_session *)handle;
    struct advc_ahb_prime_export exported;
    int saved_errno;
    (void)userdata;
    if (session == NULL || native_buffer == NULL || metadata == NULL ||
        descriptor == NULL || buffer_id == 0 ||
        session->direction != ADVC_DIRECTION_DECODE ||
        session->transport != ADVC_TRANSPORT_AHARDWAREBUFFER ||
        !session->started)
        return ADVC_STATUS_BAD_MESSAGE;
    if (session->prime_mapper == NULL) {
        session->prime_mapper = advc_android_prime_mapper_create();
        if (session->prime_mapper == NULL)
            return prime_errno_to_status(errno == 0 ? ENOTSUP : errno);
    }
    if (advc_ahb_prime_mapper_export(
            session->prime_mapper, native_buffer, metadata, buffer_id, -1,
            &exported) < 0) {
        saved_errno = errno == 0 ? ENOTSUP : errno;
        return prime_errno_to_status(saved_errno);
    }
    *descriptor = exported.descriptor;
    for (uint32_t i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        exported.descriptor.objects[i].fd = -1;
    advc_ahb_prime_export_close(&exported);
    return ADVC_STATUS_OK;
}

static uint32_t android_receive_ahb_input(
    void *userdata, void *handle, int socket_fd,
    const struct advc_backend_ahb_input *input, int *release_fence_fd) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    AHardwareBuffer *buffer = NULL;
    AHardwareBuffer_Desc desc;
    int acquire_fence;
    int render_result;
    struct pollfd handle_poll;
    struct timeval old_timeout;
    struct timeval bounded_timeout = {2, 0};
    socklen_t old_timeout_size = sizeof(old_timeout);
    int have_old_timeout = 0;
    int receive_status = -1;
    (void)userdata;
    if (input == NULL || release_fence_fd == NULL) return ADVC_STATUS_BAD_MESSAGE;
    acquire_fence = input->acquire_fence_fd;
    *release_fence_fd = -1;
    memset(&handle_poll, 0, sizeof(handle_poll));
    handle_poll.fd = socket_fd;
    handle_poll.events = POLLIN;
    do {
        render_result = poll(&handle_poll, 1, 2000);
    } while (render_result < 0 && errno == EINTR);
    if (render_result > 0 && (handle_poll.revents & POLLIN) != 0 &&
        getsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &old_timeout,
                   &old_timeout_size) == 0) {
        have_old_timeout = 1;
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &bounded_timeout,
                       sizeof(bounded_timeout)) == 0)
            receive_status = advc_receive_ahardwarebuffer(socket_fd, &buffer);
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &old_timeout,
                         old_timeout_size);
    }
    if (!have_old_timeout || receive_status != 0 || buffer == NULL) {
        if (acquire_fence >= 0) close(acquire_fence);
        if (buffer != NULL) AHardwareBuffer_release(buffer);
        return ADVC_BACKEND_AHB_FATAL_TRANSPORT;
    }
    AHardwareBuffer_describe(buffer, &desc);
    if (session == NULL ||
        session->transport != ADVC_TRANSPORT_ANDROID_AHB_SURFACE ||
        !session->started || session->input_eos_queued ||
        session->local_surface_pending >= ADVC_MAX_INFLIGHT_DMABUFS ||
        input->pts_ns > (uint64_t)INT64_MAX ||
        (session->local_surface_have_pts &&
         input->pts_ns <= session->local_surface_last_pts_ns) ||
        desc.width != input->width || desc.height != input->height ||
        desc.layers != input->layers || desc.format != input->format ||
        desc.usage != input->usage || desc.width != session->width ||
        desc.height != session->height || desc.layers != 1 ||
        desc.stride == 0 || desc.stride < desc.width ||
        desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        (desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) == 0 ||
        (desc.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) != 0 ||
        (desc.usage & ~(AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                        AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY |
                        AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN)) != 0) {
        if (acquire_fence >= 0) close(acquire_fence);
        AHardwareBuffer_release(buffer);
        return session != NULL &&
                       session->local_surface_pending >=
                           ADVC_MAX_INFLIGHT_DMABUFS ?
               ADVC_STATUS_WOULD_BLOCK : ADVC_STATUS_BAD_MESSAGE;
    }
    render_result = advc_egl_surface_producer_render_ahb(
        session->egl_producer, buffer, session->local_surface_frames,
        (int64_t)input->pts_ns, acquire_fence, release_fence_fd);
    AHardwareBuffer_release(buffer);
    if (render_result < 0) return ADVC_STATUS_UNSUPPORTED;
    ++session->local_surface_frames;
    session->local_surface_last_pts_ns = input->pts_ns;
    session->local_surface_have_pts = 1;
    ++session->local_surface_pending;
    return ADVC_STATUS_OK;
}

static int android_dmabuf_format_allowed(
    void *userdata, void *handle,
    const struct advc_dmabuf_descriptor *descriptor) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    (void)userdata;
    if (session == NULL || descriptor == NULL ||
        session->transport != ADVC_TRANSPORT_DMABUF || !session->started ||
        session->surface_transport_failed ||
        session->input_eos_queued || descriptor->crop_width != session->width ||
        descriptor->crop_height != session->height)
        return 0;
    if (session->vk_producer != NULL)
        return advc_vk_surface_producer_validate_dmabuf(
                   session->vk_producer, descriptor) == 0;
    return session->egl_producer != NULL &&
           advc_egl_surface_producer_validate_dmabuf(
               session->egl_producer, descriptor) == 0;
}

static int request_surface_sync_frame(struct android_codec_session *session) {
    AMediaFormat *parameters;
    media_status_t status;
    if (getenv("ADVC_SURFACE_REQUEST_SYNC") == NULL ||
        strcmp(getenv("ADVC_SURFACE_REQUEST_SYNC"), "validated-v1") != 0)
        return 0;
    if (session == NULL || session->codec == NULL) return -1;
    parameters = AMediaFormat_new();
    if (parameters == NULL) return -1;
    AMediaFormat_setInt32(parameters, "request-sync", 0);
    status = AMediaCodec_setParameters(session->codec, parameters);
    AMediaFormat_delete(parameters);
    if (session->debug)
        fprintf(stderr, "advc-codec: request-sync status=%d\n", (int)status);
    return status == AMEDIA_OK ? 0 : -1;
}

static uint32_t android_submit_dmabuf(
    void *userdata, void *handle,
    const struct advc_dmabuf_descriptor *descriptor, uint64_t pts_ns,
    int acquire_fence_fd, int *release_fence_fd) {
    struct android_codec_session *session = (struct android_codec_session *)handle;
    int render_result;
    (void)userdata;
    if (release_fence_fd == NULL) {
        if (acquire_fence_fd >= 0) close(acquire_fence_fd);
        return ADVC_STATUS_BAD_MESSAGE;
    }
    *release_fence_fd = -1;
    if (session == NULL || descriptor == NULL ||
        session->transport != ADVC_TRANSPORT_DMABUF || !session->started ||
        session->surface_transport_failed || session->input_eos_queued ||
        session->local_surface_pending >= ADVC_MAX_INFLIGHT_DMABUFS ||
        pts_ns > (uint64_t)INT64_MAX ||
        (session->local_surface_have_pts &&
         pts_ns <= session->local_surface_last_pts_ns) ||
        (acquire_fence_fd >= 0 &&
         advc_dmabuf_sync_file_validate(acquire_fence_fd) < 0)) {
        if (acquire_fence_fd >= 0) close(acquire_fence_fd);
        return session != NULL &&
                       session->local_surface_pending >=
                           ADVC_MAX_INFLIGHT_DMABUFS
                   ? ADVC_STATUS_WOULD_BLOCK
                   : ADVC_STATUS_BAD_MESSAGE;
    }
    if (request_surface_sync_frame(session) < 0) {
        if (acquire_fence_fd >= 0) close(acquire_fence_fd);
        return ADVC_STATUS_CODEC_ERROR;
    }
    if (session->vk_producer != NULL)
        render_result = advc_vk_surface_producer_render_dmabuf(
            session->vk_producer, descriptor, session->local_surface_frames,
            (int64_t)pts_ns, acquire_fence_fd, release_fence_fd);
    else
        render_result = advc_egl_surface_producer_render_dmabuf(
            session->egl_producer, descriptor, session->local_surface_frames,
            (int64_t)pts_ns, acquire_fence_fd, release_fence_fd);
    if (render_result < 0) {
        /*
         * An acquired swapchain image may no longer be presentable after a
         * Vulkan error. Discard the producer and poison this codec session;
         * the client must close and recreate the session, which also creates
         * a fresh MediaCodec input Surface. Never fall through to EGL on the
         * same Surface or reuse the possibly acquired image.
         */
        advc_vk_surface_producer_destroy(session->vk_producer);
        session->vk_producer = NULL;
        session->surface_transport_failed = 1;
        return ADVC_STATUS_CODEC_ERROR;
    }
    ++session->local_surface_frames;
    session->local_surface_last_pts_ns = pts_ns;
    session->local_surface_have_pts = 1;
    ++session->local_surface_pending;
    return ADVC_STATUS_OK;
}
#endif

const struct advc_backend_ops *advc_android_codec_backend_ops(void) {
    static const struct advc_backend_ops ops = {
        .create = android_create,
        .queue_input = android_queue_input,
        .dequeue_output = android_dequeue_output,
        .release_output = android_release_output,
        .send_native_buffer = android_send_native_buffer,
#if defined(__ANDROID__)
        .export_decode_prime = android_export_decode_prime,
        .receive_ahb_input = android_receive_ahb_input,
        .dmabuf_format_allowed = android_dmabuf_format_allowed,
        .submit_dmabuf = android_submit_dmabuf,
#endif
        .flush = android_flush,
        .destroy = android_destroy,
    };
    return &ops;
}
