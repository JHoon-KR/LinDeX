#include "android_codec_backend.h"

#include <assert.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 32
#define MAX_DEQUEUE_CALLS 256

void advc_test_ahb_reset(void);
int advc_test_ahb_acquire_calls(void);
int advc_test_ahb_release_calls(void);
int advc_test_ahb_discard_calls(void);
void advc_test_surface_reset(void);
int advc_test_surface_render_calls(void);
uint64_t advc_test_surface_last_frame(void);
int64_t advc_test_surface_last_pts(void);
int advc_test_surface_eos_calls(void);

struct AMediaCodec {
    int unused;
};

struct AMediaFormat {
    int32_t width;
    int32_t height;
    int32_t color_format;
    int32_t stride;
    int32_t slice_height;
    int32_t crop_left;
    int32_t crop_top;
    int32_t crop_right;
    int32_t crop_bottom;
    int32_t low_latency;
    int32_t bitrate_mode;
    float operating_rate;
    int low_latency_set;
    int bitrate_mode_set;
    int operating_rate_set;
    uint8_t csd0[64];
    size_t csd0_size;
    uint8_t csd1[64];
    size_t csd1_size;
};

struct fake_event {
    ssize_t index;
    AMediaCodecBufferInfo info;
};

static struct AMediaCodec fake_codec;
static struct AMediaFormat configured_format;
static struct AMediaFormat output_format;
static struct fake_event events[MAX_EVENTS];
static size_t event_count;
static size_t event_at;
static int64_t dequeue_timeouts[MAX_DEQUEUE_CALLS];
static size_t dequeue_call_count;
static uint8_t input_bytes[4096];
static uint8_t *output_bytes;
static size_t output_capacity;
static int configure_calls;
static int start_calls;
static int queue_calls;
static uint32_t last_queue_flags;
static size_t last_queue_size;
static int release_calls;
static size_t last_release_index;
static bool last_release_render;
static int flush_calls;

static void reset_fake(void) {
    memset(&configured_format, 0, sizeof(configured_format));
    memset(&output_format, 0, sizeof(output_format));
    memset(events, 0, sizeof(events));
    memset(dequeue_timeouts, 0, sizeof(dequeue_timeouts));
    event_count = 0;
    event_at = 0;
    dequeue_call_count = 0;
    output_bytes = NULL;
    output_capacity = 0;
    configure_calls = 0;
    start_calls = 0;
    queue_calls = 0;
    last_queue_flags = 0;
    last_queue_size = 0;
    release_calls = 0;
    last_release_index = 0;
    flush_calls = 0;
    last_release_render = false;
    advc_test_ahb_reset();
    advc_test_surface_reset();
    unsetenv("ADVC_CODEC_CONFIG_AS_DATA");
    unsetenv("ADVC_CODEC_NAME");
    unsetenv("ADVC_CODEC_DIAGNOSTIC_LOW_LATENCY");
    unsetenv("ADVC_CODEC_DIAGNOSTIC_BITRATE_MODE");
    unsetenv("ADVC_CODEC_DIAGNOSTIC_OPERATING_RATE");
    unsetenv("ADVC_CODEC_DECODER_DESTROY_DRAIN");
}

static void add_event(ssize_t index, int32_t offset, int32_t size,
                      int64_t pts_us, uint32_t flags) {
    if (event_at == event_count) {
        event_at = 0;
        event_count = 0;
    }
    assert(event_count < MAX_EVENTS);
    events[event_count].index = index;
    events[event_count].info.offset = offset;
    events[event_count].info.size = size;
    events[event_count].info.presentationTimeUs = pts_us;
    events[event_count].info.flags = flags;
    ++event_count;
}

int android_get_device_api_level(void) {
    return 36;
}

AMediaFormat *AMediaFormat_new(void) {
    return calloc(1, sizeof(struct AMediaFormat));
}

void AMediaFormat_delete(AMediaFormat *format) {
    free(format);
}

void AMediaFormat_setString(AMediaFormat *format, const char *name,
                            const char *value) {
    (void)format;
    (void)name;
    (void)value;
}

void AMediaFormat_setInt32(AMediaFormat *format, const char *name, int32_t value) {
    if (strcmp(name, AMEDIAFORMAT_KEY_WIDTH) == 0) format->width = value;
    else if (strcmp(name, AMEDIAFORMAT_KEY_HEIGHT) == 0) format->height = value;
    else if (strcmp(name, AMEDIAFORMAT_KEY_COLOR_FORMAT) == 0)
        format->color_format = value;
    else if (strcmp(name, AMEDIAFORMAT_KEY_STRIDE) == 0) format->stride = value;
    else if (strcmp(name, AMEDIAFORMAT_KEY_SLICE_HEIGHT) == 0)
        format->slice_height = value;
    else if (strcmp(name, "crop-left") == 0) format->crop_left = value;
    else if (strcmp(name, "crop-top") == 0) format->crop_top = value;
    else if (strcmp(name, "crop-right") == 0) format->crop_right = value;
    else if (strcmp(name, "crop-bottom") == 0) format->crop_bottom = value;
    else if (strcmp(name, "low-latency") == 0) {
        format->low_latency = value;
        format->low_latency_set = 1;
    } else if (strcmp(name, "bitrate-mode") == 0) {
        format->bitrate_mode = value;
        format->bitrate_mode_set = 1;
    }
}

void AMediaFormat_setFloat(AMediaFormat *format, const char *name, float value) {
    if (strcmp(name, "operating-rate") == 0) {
        format->operating_rate = value;
        format->operating_rate_set = 1;
    }
}

bool AMediaFormat_getBuffer(AMediaFormat *format, const char *name, void **data,
                            size_t *size) {
    if (strcmp(name, "csd-0") == 0 && format->csd0_size > 0) {
        *data = format->csd0;
        *size = format->csd0_size;
        return true;
    }
    if (strcmp(name, "csd-1") == 0 && format->csd1_size > 0) {
        *data = format->csd1;
        *size = format->csd1_size;
        return true;
    }
    *data = NULL;
    *size = 0;
    return false;
}

void AMediaFormat_setBuffer(AMediaFormat *format, const char *name, const void *data,
                            size_t size) {
    uint8_t *destination;
    size_t *stored_size;
    assert(size <= 64);
    if (strcmp(name, "csd-0") == 0) {
        destination = format->csd0;
        stored_size = &format->csd0_size;
    } else {
        assert(strcmp(name, "csd-1") == 0);
        destination = format->csd1;
        stored_size = &format->csd1_size;
    }
    memcpy(destination, data, size);
    *stored_size = size;
}

bool AMediaFormat_getInt32(AMediaFormat *format, const char *name, int32_t *value) {
    if (strcmp(name, AMEDIAFORMAT_KEY_WIDTH) == 0) *value = format->width;
    else if (strcmp(name, AMEDIAFORMAT_KEY_HEIGHT) == 0) *value = format->height;
    else if (strcmp(name, AMEDIAFORMAT_KEY_COLOR_FORMAT) == 0)
        *value = format->color_format;
    else if (strcmp(name, AMEDIAFORMAT_KEY_STRIDE) == 0) *value = format->stride;
    else if (strcmp(name, AMEDIAFORMAT_KEY_SLICE_HEIGHT) == 0)
        *value = format->slice_height;
    else if (strcmp(name, "crop-left") == 0) *value = format->crop_left;
    else if (strcmp(name, "crop-top") == 0) *value = format->crop_top;
    else if (strcmp(name, "crop-right") == 0) *value = format->crop_right;
    else if (strcmp(name, "crop-bottom") == 0) *value = format->crop_bottom;
    else return false;
    return true;
}

AMediaCodec *AMediaCodec_createCodecByName(const char *name) {
    (void)name;
    return &fake_codec;
}

AMediaCodec *AMediaCodec_createEncoderByType(const char *mime) {
    (void)mime;
    return &fake_codec;
}

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime) {
    (void)mime;
    return &fake_codec;
}

media_status_t AMediaCodec_delete(AMediaCodec *codec) {
    assert(codec == &fake_codec);
    return AMEDIA_OK;
}

media_status_t AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *format,
                                     void *surface, void *crypto, uint32_t flags) {
    (void)surface;
    (void)crypto;
    (void)flags;
    assert(codec == &fake_codec);
    configured_format = *format;
    ++configure_calls;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec *codec) {
    assert(codec == &fake_codec);
    ++start_calls;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_stop(AMediaCodec *codec) {
    assert(codec == &fake_codec);
    return AMEDIA_OK;
}

media_status_t AMediaCodec_flush(AMediaCodec *codec) {
    assert(codec == &fake_codec);
    ++flush_calls;
    return AMEDIA_OK;
}

media_status_t AMediaCodec_setParameters(AMediaCodec *codec,
                                         const AMediaFormat *params) {
    assert(codec == &fake_codec);
    assert(params != NULL);
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeout_us) {
    assert(codec == &fake_codec);
    assert(timeout_us == 0);
    return 0;
}

uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t index,
                                    size_t *capacity) {
    assert(codec == &fake_codec);
    assert(index == 0);
    *capacity = sizeof(input_bytes);
    return input_bytes;
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t index,
                                            off_t offset, size_t size,
                                            uint64_t presentation_time_us,
                                            uint32_t flags) {
    (void)presentation_time_us;
    assert(codec == &fake_codec);
    assert(index == 0);
    assert(offset == 0);
    ++queue_calls;
    last_queue_flags = flags;
    last_queue_size = size;
    return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec,
                                        AMediaCodecBufferInfo *info,
                                        int64_t timeout_us) {
    assert(codec == &fake_codec);
    assert(dequeue_call_count < MAX_DEQUEUE_CALLS);
    dequeue_timeouts[dequeue_call_count++] = timeout_us;
    if (event_at >= event_count) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    *info = events[event_at].info;
    return events[event_at++].index;
}

uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t index,
                                     size_t *capacity) {
    (void)index;
    assert(codec == &fake_codec);
    *capacity = output_capacity;
    return output_bytes;
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t index,
                                               bool render) {
    assert(codec == &fake_codec);
    ++release_calls;
    last_release_index = index;
    last_release_render = render;
    return AMEDIA_OK;
}

AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec) {
    AMediaFormat *format;
    assert(codec == &fake_codec);
    format = AMediaFormat_new();
    assert(format != NULL);
    *format = output_format;
    return format;
}

static void create_decoder(const struct advc_backend_ops *ops, void **handle) {
    struct advc_backend_config config;
    memset(&config, 0, sizeof(config));
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 640;
    config.height = 360;
    config.framerate_milli = 30000;
    strcpy(config.mime, "video/avc");
    assert(ops->create(NULL, &config, handle) == ADVC_STATUS_OK);
    assert(*handle != NULL);
}

static void create_ahb_decoder(const struct advc_backend_ops *ops, void **handle) {
    struct advc_backend_config config;
    memset(&config, 0, sizeof(config));
    config.direction = ADVC_DIRECTION_DECODE;
    config.width = 640;
    config.height = 360;
    config.framerate_milli = 30000;
    config.transport = ADVC_TRANSPORT_AHARDWAREBUFFER;
    strcpy(config.mime, "video/avc");
    assert(ops->create(NULL, &config, handle) == ADVC_STATUS_OK);
    assert(*handle != NULL);
}

static void test_surface_empty_eos_is_control_output(void) {
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_output output;
    void *handle = NULL;

    reset_fake();
    create_ahb_decoder(ops, &handle);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"x", 1, 1000, 0) ==
           ADVC_STATUS_OK);
    add_event(7, 0, 0, 2, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.transport == ADVC_TRANSPORT_BYTES);
    assert(output.data == NULL && output.size == 0);
    assert(output.flags == ADVC_FLAG_END_OF_STREAM);
    assert(output.pts_ns == 2000);
    assert(output.width == 640 && output.height == 360);
    assert(release_calls == 1 && last_release_index == 7 && !last_release_render);
    assert(advc_test_ahb_acquire_calls() == 0);
    ops->release_output(NULL, handle, output.token, -1);
    assert(advc_test_ahb_release_calls() == 0);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_WOULD_BLOCK);
    assert(dequeue_call_count == 1);

    assert(ops->flush(NULL, handle) == ADVC_STATUS_OK);
    assert(advc_test_ahb_discard_calls() == 1);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"y", 1, 3000, 0) ==
           ADVC_STATUS_OK);
    add_event(8, 0, 0, 4, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.transport == ADVC_TRANSPORT_BYTES);
    assert(output.flags == ADVC_FLAG_END_OF_STREAM);
    assert(release_calls == 2 && last_release_index == 8 && !last_release_render);
    assert(advc_test_ahb_acquire_calls() == 0);
    ops->release_output(NULL, handle, output.token, -1);
    ops->destroy(NULL, handle);
}

static void test_surface_destroy_gate_off_preserves_stock_teardown(void) {
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    void *handle = NULL;

    reset_fake();
    create_ahb_decoder(ops, &handle);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"x", 1, 1000, 0) ==
           ADVC_STATUS_OK);
    assert(advc_test_ahb_discard_calls() == 0);
    ops->destroy(NULL, handle);
    assert(advc_test_ahb_discard_calls() == 0);
    assert(dequeue_call_count == 0);
    assert(release_calls == 0);
}

static void test_surface_destroy_can_discard_ready_codec_outputs(void) {
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    void *handle = NULL;

    reset_fake();
    create_ahb_decoder(ops, &handle);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"x", 1, 1000, 0) ==
           ADVC_STATUS_OK);
    add_event(9, 0, 8, 1, 0);
    assert(setenv("ADVC_CODEC_DECODER_DESTROY_DRAIN",
                  "validated-bounded-v1", 1) == 0);
    ops->destroy(NULL, handle);
    assert(release_calls == 1);
    assert(last_release_index == 9);
    assert(!last_release_render);
    assert(dequeue_call_count == 2);
    assert(dequeue_timeouts[0] == 0 && dequeue_timeouts[1] == 0);
    assert(advc_test_ahb_discard_calls() == 2);
}

static void test_decoder_csd_output_events_and_eos(void) {
    static const uint8_t csd[] = {
        0, 0, 0, 1, 0x67, 0xaa,
        0, 0, 0, 1, 0x68, 0xbb,
    };
    static const uint8_t frame[] = {0, 0, 1, 0x65, 0x11};
    static uint8_t bytes[] = {9, 1, 2, 3, 9};
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_output output;
    void *handle = NULL;

    reset_fake();
    create_decoder(ops, &handle);
    assert(configure_calls == 0);
    assert(start_calls == 0);
    assert(ops->queue_input(NULL, handle, csd, sizeof(csd), 0,
                            ADVC_FLAG_CODEC_CONFIG) == ADVC_STATUS_OK);
    assert(configure_calls == 1);
    assert(start_calls == 1);
    assert(queue_calls == 0);
    assert(configured_format.csd0_size == 6);
    assert(configured_format.csd1_size == 6);
    assert(memcmp(configured_format.csd0, csd, 6) == 0);
    assert(memcmp(configured_format.csd1, csd + 6, 6) == 0);

    assert(ops->queue_input(NULL, handle, frame, sizeof(frame), 1000,
                            ADVC_FLAG_KEY_FRAME) == ADVC_STATUS_OK);
    assert(queue_calls == 1);
    assert(last_queue_flags == 0);
    assert(last_queue_size == sizeof(frame));
    assert(ops->queue_input(NULL, handle, NULL, 0, 2000,
                            ADVC_FLAG_END_OF_STREAM) == ADVC_STATUS_OK);
    assert(queue_calls == 2);
    assert(last_queue_flags == AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    assert(ops->queue_input(NULL, handle, frame, sizeof(frame), 3000, 0) ==
           ADVC_STATUS_BAD_MESSAGE);

    output_format.width = 640;
    output_format.height = 360;
    output_format.color_format = 0x7f420888;
    output_format.stride = 672;
    output_format.slice_height = 368;
    output_format.crop_left = 2;
    output_format.crop_top = 4;
    output_format.crop_right = 637;
    output_format.crop_bottom = 355;
    output_bytes = bytes;
    output_capacity = sizeof(bytes);
    add_event(AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED, 0, 0, 0, 0);
    add_event(AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED, 0, 0, 0, 0);
    add_event(2, 1, 3, 7, AMEDIACODEC_BUFFER_FLAG_KEY_FRAME);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(dequeue_call_count == 3);
    assert(dequeue_timeouts[0] == 10000);
    assert(dequeue_timeouts[1] == 0);
    assert(dequeue_timeouts[2] == 0);
    assert(output.data == bytes + 1);
    assert(output.size == 3);
    assert(output.pts_ns == 7000);
    assert(output.flags == ADVC_FLAG_KEY_FRAME);
    assert(output.width == 640 && output.height == 360);
    assert(output.android_format == 0x7f420888u);
    assert(output.stride == 672 && output.slice_height == 368);
    assert(output.crop_left == 2 && output.crop_top == 4);
    assert(output.crop_right == 637 && output.crop_bottom == 355);
    ops->release_output(NULL, handle, output.token, -1);
    assert(release_calls == 1 && last_release_index == 2);

    output_bytes = NULL;
    output_capacity = 0;
    add_event(3, 0, 0, 8, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.size == 0 && output.data == NULL);
    assert(output.flags == ADVC_FLAG_END_OF_STREAM);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_WOULD_BLOCK);
    assert(dequeue_call_count == 4);
    assert(ops->flush(NULL, handle) == ADVC_STATUS_OK);
    assert(flush_calls == 1);
    assert(ops->queue_input(NULL, handle, frame, sizeof(frame), 4000, 0) ==
           ADVC_STATUS_OK);
    ops->destroy(NULL, handle);
}

static void test_bounded_poll_and_event_spin(void) {
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_output output;
    void *handle = NULL;

    reset_fake();
    create_decoder(ops, &handle);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"x", 1, 0, 0) ==
           ADVC_STATUS_OK);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_WOULD_BLOCK);
    assert(dequeue_call_count == 1);
    assert(dequeue_timeouts[0] == 10000);

    dequeue_call_count = 0;
    for (size_t i = 0; i < 8; ++i)
        add_event(AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED, 0, 0, 0, 0);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_WOULD_BLOCK);
    assert(dequeue_call_count == 8);
    assert(dequeue_timeouts[0] == 10000);
    for (size_t i = 1; i < dequeue_call_count; ++i)
        assert(dequeue_timeouts[i] == 0);
    ops->destroy(NULL, handle);
}

static void test_encoder_config_output(void) {
    static const uint8_t csd0[] = {0, 0, 0, 1, 0x67};
    static const uint8_t csd1[] = {0, 0, 0, 1, 0x68};
    static uint8_t bytes[] = {0, 0, 0, 1, 0x65};
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_config config;
    struct advc_backend_output output;
    void *handle = NULL;

    reset_fake();
    memset(&config, 0, sizeof(config));
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 128;
    config.height = 64;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.color_format = ADVC_COLOR_FORMAT_YUV420_PLANAR;
    strcpy(config.mime, "video/avc");
    assert(ops->create(NULL, &config, &handle) == ADVC_STATUS_OK);
    assert(configure_calls == 1 && start_calls == 1);
    assert(configured_format.color_format == ADVC_COLOR_FORMAT_YUV420_PLANAR);

    memcpy(output_format.csd0, csd0, sizeof(csd0));
    output_format.csd0_size = sizeof(csd0);
    memcpy(output_format.csd1, csd1, sizeof(csd1));
    output_format.csd1_size = sizeof(csd1);
    add_event(AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED, 0, 0, 0, 0);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.flags == ADVC_FLAG_CODEC_CONFIG);
    assert(output.size == sizeof(csd0) + sizeof(csd1));
    assert(memcmp(output.data, csd0, sizeof(csd0)) == 0);
    assert(memcmp(output.data + sizeof(csd0), csd1, sizeof(csd1)) == 0);
    assert(output.width == 128 && output.height == 64);
    assert(output.android_format == 0 && output.stride == 0 &&
           output.slice_height == 0);
    ops->release_output(NULL, handle, output.token, -1);
    assert(release_calls == 0);

    output_bytes = bytes;
    output_capacity = sizeof(bytes);
    add_event(5, 0, (int32_t)sizeof(bytes), 1,
              AMEDIACODEC_BUFFER_FLAG_KEY_FRAME);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.flags == ADVC_FLAG_KEY_FRAME);
    assert(output.size == sizeof(bytes));
    ops->release_output(NULL, handle, output.token, -1);
    assert(release_calls == 1 && last_release_index == 5);
    ops->destroy(NULL, handle);
}

static void test_broker_local_egl_surface_encoder(void) {
    static uint8_t encoded[] = {0, 0, 0, 1, 0x65, 0x33};
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_config config;
    struct advc_backend_output output;
    void *handle = NULL;

    reset_fake();
    memset(&config, 0, sizeof(config));
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 128;
    config.height = 64;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.transport = ADVC_TRANSPORT_BROKER_EGL_SURFACE;
    strcpy(config.mime, "video/avc");
    assert(ops->create(NULL, &config, &handle) == ADVC_STATUS_OK);
    assert(handle != NULL);
    assert(configure_calls == 1 && start_calls == 1);
    assert(configured_format.color_format == (int32_t)0x7f000789u);

    assert(ops->queue_input(NULL, handle, NULL, 0, 123456, 0) ==
           ADVC_STATUS_OK);
    assert(ops->queue_input(NULL, handle, NULL, 0, 234567, 0) ==
           ADVC_STATUS_OK);
    assert(ops->queue_input(NULL, handle, NULL, 0, 345678, 0) ==
           ADVC_STATUS_OK);
    assert(ops->queue_input(NULL, handle, NULL, 0, 456789, 0) ==
           ADVC_STATUS_OK);
    assert(ops->queue_input(NULL, handle, NULL, 0, 567890, 0) ==
           ADVC_STATUS_WOULD_BLOCK);
    assert(advc_test_surface_render_calls() == ADVC_MAX_INFLIGHT_DMABUFS);
    assert(advc_test_surface_last_frame() ==
           ADVC_MAX_INFLIGHT_DMABUFS - 1u);
    assert(advc_test_surface_last_pts() == 456789);
    assert(queue_calls == 0);
    assert(ops->queue_input(NULL, handle, NULL, 0, 123456, 0) ==
           ADVC_STATUS_BAD_MESSAGE);
    assert(ops->queue_input(NULL, handle, (const uint8_t *)"x", 1, 0, 0) ==
           ADVC_STATUS_BAD_MESSAGE);

    output_bytes = encoded;
    output_capacity = sizeof(encoded);
    add_event(8, 0, (int32_t)sizeof(encoded), 0,
              AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.flags == ADVC_FLAG_CODEC_CONFIG);
    ops->release_output(NULL, handle, output.token, -1);
    assert(ops->queue_input(NULL, handle, NULL, 0, 567890, 0) ==
           ADVC_STATUS_WOULD_BLOCK);

    add_event(9, 0, (int32_t)sizeof(encoded), 5,
              AMEDIACODEC_BUFFER_FLAG_KEY_FRAME);
    assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
    assert(output.size == sizeof(encoded));
    assert(output.flags == ADVC_FLAG_KEY_FRAME);
    ops->release_output(NULL, handle, output.token, -1);
    assert(last_release_index == 9 && !last_release_render);

    assert(ops->queue_input(NULL, handle, NULL, 0, 567890, 0) ==
           ADVC_STATUS_OK);
    assert(advc_test_surface_render_calls() ==
           ADVC_MAX_INFLIGHT_DMABUFS + 1u);

    /*
     * The production producer must not inherit the bounded smoke-probe limit.
     * Keep draining one coded output per input so this test reaches well past
     * the historical 120-frame failure without exceeding the inflight bound.
     */
    for (uint64_t frame = ADVC_MAX_INFLIGHT_DMABUFS + 1u;
         frame <= 130u; ++frame) {
        add_event((ssize_t)(10u + frame), 0, (int32_t)sizeof(encoded),
                  (int64_t)frame, AMEDIACODEC_BUFFER_FLAG_KEY_FRAME);
        assert(ops->dequeue_output(NULL, handle, &output) == ADVC_STATUS_OK);
        ops->release_output(NULL, handle, output.token, -1);
        assert(ops->queue_input(NULL, handle, NULL, 0,
                                567890u + frame, 0) == ADVC_STATUS_OK);
    }
    assert(advc_test_surface_render_calls() == 131);
    assert(advc_test_surface_last_frame() == 130);

    assert(ops->queue_input(NULL, handle, NULL, 0, 678901,
                            ADVC_FLAG_END_OF_STREAM) == ADVC_STATUS_OK);
    assert(advc_test_surface_eos_calls() == 1);
    assert(ops->queue_input(NULL, handle, NULL, 0, 567890, 0) ==
           ADVC_STATUS_BAD_MESSAGE);
    ops->destroy(NULL, handle);
}

static void test_encoder_diagnostic_format_is_exact_opt_in(void) {
    const struct advc_backend_ops *ops = advc_android_codec_backend_ops();
    struct advc_backend_config config;
    void *handle = NULL;

    reset_fake();
    memset(&config, 0, sizeof(config));
    config.direction = ADVC_DIRECTION_ENCODE;
    config.encode_profile = ADVC_ENCODE_PROFILE_H264_CONSTRAINED_BASELINE;
    config.width = 128;
    config.height = 64;
    config.bitrate = 1000000;
    config.framerate_milli = 30000;
    config.color_format = ADVC_COLOR_FORMAT_YUV420_PLANAR;
    strcpy(config.mime, "video/avc");
    setenv("ADVC_CODEC_DIAGNOSTIC_LOW_LATENCY", "validated-1-v1", 1);
    setenv("ADVC_CODEC_DIAGNOSTIC_BITRATE_MODE", "validated-cbr-v1", 1);
    setenv("ADVC_CODEC_DIAGNOSTIC_OPERATING_RATE", "validated-120-v1", 1);
    assert(ops->create(NULL, &config, &handle) == ADVC_STATUS_OK);
    assert(configured_format.low_latency_set &&
           configured_format.low_latency == 1);
    assert(configured_format.bitrate_mode_set &&
           configured_format.bitrate_mode == 2);
    assert(configured_format.operating_rate_set &&
           configured_format.operating_rate == 120.0f);
    ops->destroy(NULL, handle);

    reset_fake();
    handle = NULL;
    setenv("ADVC_CODEC_DIAGNOSTIC_LOW_LATENCY", "1", 1);
    setenv("ADVC_CODEC_DIAGNOSTIC_BITRATE_MODE", "cbr", 1);
    setenv("ADVC_CODEC_DIAGNOSTIC_OPERATING_RATE", "120", 1);
    assert(ops->create(NULL, &config, &handle) == ADVC_STATUS_OK);
    assert(!configured_format.low_latency_set);
    assert(configured_format.bitrate_mode_set &&
           configured_format.bitrate_mode == 1);
    assert(!configured_format.operating_rate_set);
    ops->destroy(NULL, handle);
}

int main(void) {
    test_encoder_diagnostic_format_is_exact_opt_in();
    test_broker_local_egl_surface_encoder();
    test_surface_destroy_gate_off_preserves_stock_teardown();
    test_surface_destroy_can_discard_ready_codec_outputs();
    test_surface_empty_eos_is_control_output();
    test_decoder_csd_output_events_and_eos();
    test_bounded_poll_and_event_spin();
    test_encoder_config_output();
    puts("android codec backend tests: PASS");
    return 0;
}
