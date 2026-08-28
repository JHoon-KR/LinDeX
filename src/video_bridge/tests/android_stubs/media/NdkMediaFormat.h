#ifndef ADVC_TEST_NDK_MEDIA_FORMAT_H
#define ADVC_TEST_NDK_MEDIA_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AMediaFormat AMediaFormat;

#define AMEDIAFORMAT_KEY_MIME "mime"
#define AMEDIAFORMAT_KEY_WIDTH "width"
#define AMEDIAFORMAT_KEY_HEIGHT "height"
#define AMEDIAFORMAT_KEY_FRAME_RATE "frame-rate"
#define AMEDIAFORMAT_KEY_BIT_RATE "bitrate"
#define AMEDIAFORMAT_KEY_COLOR_FORMAT "color-format"
#define AMEDIAFORMAT_KEY_I_FRAME_INTERVAL "i-frame-interval"
#define AMEDIAFORMAT_KEY_STRIDE "stride"
#define AMEDIAFORMAT_KEY_SLICE_HEIGHT "slice-height"

AMediaFormat *AMediaFormat_new(void);
void AMediaFormat_delete(AMediaFormat *format);
void AMediaFormat_setString(AMediaFormat *format, const char *name,
                            const char *value);
void AMediaFormat_setInt32(AMediaFormat *format, const char *name, int32_t value);
void AMediaFormat_setFloat(AMediaFormat *format, const char *name, float value);
bool AMediaFormat_getBuffer(AMediaFormat *format, const char *name, void **data,
                            size_t *size);
void AMediaFormat_setBuffer(AMediaFormat *format, const char *name, const void *data,
                            size_t size);
bool AMediaFormat_getInt32(AMediaFormat *format, const char *name, int32_t *value);

#endif
