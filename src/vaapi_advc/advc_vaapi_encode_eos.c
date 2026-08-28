#include "advc_vaapi_encode_eos.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>

static int fail_once(struct advc_vaapi_encode_eos_state *state,
                     const struct advc_vaapi_encode_eos_ops *ops,
                     int error) {
    if (error == 0) error = EIO;
    if (!state->failed_closed) {
        state->failed_closed = 1;
        state->failure_errno = error;
        if (ops->fail_closed != NULL) ops->fail_closed(ops->opaque);
    }
    errno = state->failure_errno;
    return -1;
}

int advc_vaapi_encode_eos_finish(
    struct advc_vaapi_encode_eos_state *state, uint64_t pts_ns,
    uint32_t timeout_ms, const struct advc_vaapi_encode_eos_ops *ops) {
    uint64_t deadline;
    uint64_t now;
    if (state == NULL || ops == NULL || ops->now_ms == NULL ||
        ops->signal_eos == NULL || ops->drain_once == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (state->failed_closed)
        return fail_once(state, ops, state->failure_errno);
    if (state->eos_received) return 0;
    if (!state->signal_attempted) {
        int saved;
        state->signal_attempted = 1;
        if (ops->signal_eos(ops->opaque, pts_ns) < 0) {
            saved = errno;
            return fail_once(state, ops, saved);
        }
        state->signal_succeeded = 1;
    } else if (!state->signal_succeeded) {
        return fail_once(state, ops, EIO);
    }

    now = ops->now_ms(ops->opaque);
    if (now == UINT64_MAX || UINT64_MAX - now < timeout_ms)
        return fail_once(state, ops, EOVERFLOW);
    deadline = now + timeout_ms;
    while (!state->eos_received) {
        uint64_t remaining;
        uint32_t wait_ms;
        int received = 0;
        int saved;
        now = ops->now_ms(ops->opaque);
        if (now == UINT64_MAX)
            return fail_once(state, ops, errno == 0 ? EIO : errno);
        if (now >= deadline)
            return fail_once(state, ops, ETIMEDOUT);
        remaining = deadline - now;
        wait_ms = remaining > UINT32_MAX ? UINT32_MAX
                                         : (uint32_t)remaining;
        if (wait_ms == 0) wait_ms = 1;
        if (ops->drain_once(ops->opaque, wait_ms, &received) < 0) {
            saved = errno;
            return fail_once(state, ops, saved);
        }
        if (received) state->eos_received = 1;
    }
    return 0;
}
