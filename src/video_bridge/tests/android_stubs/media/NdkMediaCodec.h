#ifndef ADVC_TEST_NDK_MEDIA_CODEC_H
#define ADVC_TEST_NDK_MEDIA_CODEC_H

#include "NdkMediaFormat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int32_t media_status_t;
typedef struct AMediaCodec AMediaCodec;

typedef struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

#define AMEDIA_OK 0
#define AMEDIA_ERROR_UNSUPPORTED (-1010)
#define AMEDIA_ERROR_INVALID_PARAMETER (-1003)
#define AMEDIACODEC_INFO_TRY_AGAIN_LATER (-1)
#define AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1u
#define AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG 2u
#define AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM 4u
#define AMEDIACODEC_CONFIGURE_FLAG_ENCODE 1u

AMediaCodec *AMediaCodec_createCodecByName(const char *name);
AMediaCodec *AMediaCodec_createEncoderByType(const char *mime);
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime);
media_status_t AMediaCodec_delete(AMediaCodec *codec);
media_status_t AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *format,
                                     void *surface, void *crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec *codec);
media_status_t AMediaCodec_stop(AMediaCodec *codec);
media_status_t AMediaCodec_flush(AMediaCodec *codec);
media_status_t AMediaCodec_setParameters(AMediaCodec *codec,
                                         const AMediaFormat *params);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeout_us);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t index,
                                    size_t *capacity);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t index,
                                            off_t offset, size_t size,
                                            uint64_t presentation_time_us,
                                            uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec,
                                        AMediaCodecBufferInfo *info,
                                        int64_t timeout_us);
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t index,
                                     size_t *capacity);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t index,
                                               bool render);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec);

#endif
