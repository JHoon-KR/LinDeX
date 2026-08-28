#ifndef ADVC_ANDROID_CODEC_BACKEND_H
#define ADVC_ANDROID_CODEC_BACKEND_H

#include "advc/session_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

const struct advc_backend_ops *advc_android_codec_backend_ops(void);

#ifdef __cplusplus
}
#endif

#endif
