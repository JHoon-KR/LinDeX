#ifndef ADVC_VAAPI_ENCODE_EOS_H
#define ADVC_VAAPI_ENCODE_EOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct advc_vaapi_encode_eos_state {
    int signal_attempted;
    int signal_succeeded;
    int eos_received;
    int failed_closed;
    int failure_errno;
};

struct advc_vaapi_encode_eos_ops {
    void *opaque;
    uint64_t (*now_ms)(void *opaque);
    int (*signal_eos)(void *opaque, uint64_t pts_ns);
    int (*drain_once)(void *opaque, uint32_t timeout_ms,
                      int *eos_received);
    void (*fail_closed)(void *opaque);
};

/* Signal an encoder EOS once and drain through the matching EOS output.
 * timeout_ms is one total drain budget, not a per-output timeout.  Once a
 * failure is reported the state is fail-closed and later calls cannot signal
 * or drain the same session again. */
int advc_vaapi_encode_eos_finish(
    struct advc_vaapi_encode_eos_state *state, uint64_t pts_ns,
    uint32_t timeout_ms, const struct advc_vaapi_encode_eos_ops *ops);

#ifdef __cplusplus
}
#endif

#endif
