#include "advc_vaapi_encode_eos.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

struct fake_eos {
    uint64_t now_ms;
    int signal_calls;
    int drain_calls;
    int fail_calls;
    int eos_after_drains;
    int signal_errno;
    int drain_errno;
    uint64_t signaled_pts_ns;
    uint32_t last_timeout_ms;
};

static uint64_t fake_now_ms(void *opaque) {
    return ((struct fake_eos *)opaque)->now_ms;
}

static int fake_signal(void *opaque, uint64_t pts_ns) {
    struct fake_eos *fake = opaque;
    ++fake->signal_calls;
    fake->signaled_pts_ns = pts_ns;
    if (fake->signal_errno != 0) {
        errno = fake->signal_errno;
        return -1;
    }
    return 0;
}

static int fake_drain(void *opaque, uint32_t timeout_ms, int *eos_received) {
    struct fake_eos *fake = opaque;
    ++fake->drain_calls;
    fake->last_timeout_ms = timeout_ms;
    ++fake->now_ms;
    if (fake->drain_errno != 0) {
        errno = fake->drain_errno;
        return -1;
    }
    *eos_received = fake->drain_calls >= fake->eos_after_drains;
    return 0;
}

static void fake_fail_closed(void *opaque) {
    ++((struct fake_eos *)opaque)->fail_calls;
}

static struct advc_vaapi_encode_eos_ops fake_ops(struct fake_eos *fake) {
    struct advc_vaapi_encode_eos_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.opaque = fake;
    ops.now_ms = fake_now_ms;
    ops.signal_eos = fake_signal;
    ops.drain_once = fake_drain;
    ops.fail_closed = fake_fail_closed;
    return ops;
}

static void test_normal_eos_and_drain(void) {
    struct advc_vaapi_encode_eos_state state;
    struct advc_vaapi_encode_eos_ops ops;
    struct fake_eos fake;
    memset(&state, 0, sizeof(state));
    memset(&fake, 0, sizeof(fake));
    fake.now_ms = 100;
    fake.eos_after_drains = 2;
    ops = fake_ops(&fake);
    assert(advc_vaapi_encode_eos_finish(&state, 123456, 50, &ops) == 0);
    assert(fake.signal_calls == 1 && fake.signaled_pts_ns == 123456);
    assert(fake.drain_calls == 2 && fake.last_timeout_ms <= 50);
    assert(fake.fail_calls == 0);
    assert(state.signal_attempted && state.signal_succeeded &&
           state.eos_received && !state.failed_closed);
}

static void test_timeout_is_fail_closed(void) {
    struct advc_vaapi_encode_eos_state state;
    struct advc_vaapi_encode_eos_ops ops;
    struct fake_eos fake;
    memset(&state, 0, sizeof(state));
    memset(&fake, 0, sizeof(fake));
    fake.now_ms = 200;
    fake.eos_after_drains = 100;
    ops = fake_ops(&fake);
    assert(advc_vaapi_encode_eos_finish(&state, 9, 2, &ops) < 0);
    assert(errno == ETIMEDOUT);
    assert(fake.signal_calls == 1 && fake.drain_calls == 2);
    assert(fake.fail_calls == 1 && state.failed_closed);
    assert(advc_vaapi_encode_eos_finish(&state, 9, 2, &ops) < 0);
    assert(errno == ETIMEDOUT);
    assert(fake.signal_calls == 1 && fake.drain_calls == 2 &&
           fake.fail_calls == 1);
}

static void test_signal_failure_is_not_retried(void) {
    struct advc_vaapi_encode_eos_state state;
    struct advc_vaapi_encode_eos_ops ops;
    struct fake_eos fake;
    memset(&state, 0, sizeof(state));
    memset(&fake, 0, sizeof(fake));
    fake.signal_errno = EPIPE;
    ops = fake_ops(&fake);
    assert(advc_vaapi_encode_eos_finish(&state, 77, 10, &ops) < 0);
    assert(errno == EPIPE);
    assert(fake.signal_calls == 1 && fake.drain_calls == 0 &&
           fake.fail_calls == 1);
    assert(advc_vaapi_encode_eos_finish(&state, 77, 10, &ops) < 0);
    assert(errno == EPIPE);
    assert(fake.signal_calls == 1 && fake.drain_calls == 0 &&
           fake.fail_calls == 1);
}

static void test_success_is_idempotent(void) {
    struct advc_vaapi_encode_eos_state state;
    struct advc_vaapi_encode_eos_ops ops;
    struct fake_eos fake;
    memset(&state, 0, sizeof(state));
    memset(&fake, 0, sizeof(fake));
    fake.eos_after_drains = 1;
    ops = fake_ops(&fake);
    assert(advc_vaapi_encode_eos_finish(&state, 88, 10, &ops) == 0);
    assert(advc_vaapi_encode_eos_finish(&state, 88, 10, &ops) == 0);
    assert(fake.signal_calls == 1 && fake.drain_calls == 1 &&
           fake.fail_calls == 0);
}

int main(void) {
    test_normal_eos_and_drain();
    test_timeout_is_fail_closed();
    test_signal_failure_is_not_retried();
    test_success_is_idempotent();
    return 0;
}
