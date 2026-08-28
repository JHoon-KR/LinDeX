#ifndef ADVC_VAAPI_DECODE_EOS_PRIVATE_H
#define ADVC_VAAPI_DECODE_EOS_PRIVATE_H

#include "advc_vaapi_decode_eos.h"

#include <va/va.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Caller ABI returned by the versioned private getter.  signal/progress flags
 * are reserved in ABI 1.0 and must be zero.  max_outputs is in [1, 8]; one is
 * recommended so a framework can push and release each newly-ready surface
 * before it asks the driver to dequeue again.
 */
typedef int32_t (*advc_vaapi_decode_eos_signal_v1_fn)(
    VADisplay display, VAContextID context_id, uint32_t flags,
    struct advc_vaapi_decode_eos_status_v1 *status);
typedef int32_t (*advc_vaapi_decode_eos_progress_v1_fn)(
    VADisplay display, VAContextID context_id, uint32_t max_outputs,
    uint32_t flags, struct advc_vaapi_decode_eos_status_v1 *status);

enum advc_vaapi_decode_eos_capability {
    ADVC_VAAPI_DECODE_EOS_CAP_SIGNAL_PROGRESS_SPLIT = 1u << 0,
    ADVC_VAAPI_DECODE_EOS_CAP_CONTROL_EOS_NO_PTS_MATCH = 1u << 1,
    ADVC_VAAPI_DECODE_EOS_CAP_BOUNDED_PROGRESS = 1u << 2,
};

struct advc_vaapi_decode_eos_interface_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t reserved0;
    advc_vaapi_decode_eos_signal_v1_fn signal;
    advc_vaapi_decode_eos_progress_v1_fn progress;
    uintptr_t reserved[8];
};

typedef const struct advc_vaapi_decode_eos_interface_v1 *
    (*advc_vaapi_decode_eos_get_interface_v1_fn)(void);

/* Exported by advc_drv_video.so; normally resolved through vaGetLibFunc(). */
const struct advc_vaapi_decode_eos_interface_v1 *
advcVaGetDecodeEosInterface_1_0(void);

#ifdef __cplusplus
}
#endif

#endif
