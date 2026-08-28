#ifndef ADVC_MEDIA_TIME_H
#define ADVC_MEDIA_TIME_H

#include <stdint.h>

/* MediaCodec accepts microseconds and reports microseconds back to the broker. */
static inline uint64_t advc_media_codec_roundtrip_pts_ns(uint64_t pts_ns) {
    return (pts_ns / UINT64_C(1000)) * UINT64_C(1000);
}

#endif
