#define _POSIX_C_SOURCE 200809L

#include "advc_vaapi_decode_eos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

struct mock_item {
    int32_t result;
    struct advc_vaapi_decode_eos_output output;
};

struct mock_broker {
    int32_t signal_results[4];
    size_t signal_result_count;
    size_t signal_result_index;
    struct mock_item items[16];
    size_t item_count;
    size_t item_index;
    uint32_t window_size;
    uint32_t outstanding_leases;
    int fail_frame;
    int fail_release;
    int retain_frames;
    uint32_t signal_calls;
    uint32_t dequeue_calls;
    uint32_t frame_calls;
    uint32_t release_calls;
    uint64_t handled_ids[16];
    uint64_t released_ids[16];
    uint32_t terminal_session;
    uint32_t terminal_calls;
    int frame_surface_index;
    uint32_t surface_sessions[4];
    uint32_t surface_states[4];
};

enum mock_surface_state {
    MOCK_SURFACE_UNUSED = 0,
    MOCK_SURFACE_PENDING = 1,
    MOCK_SURFACE_READY = 2,
    MOCK_SURFACE_ERROR = 3,
};

static struct advc_vaapi_decode_eos_status_v1 fresh_status(void) {
    struct advc_vaapi_decode_eos_status_v1 status;
    memset(&status, 0, sizeof(status));
    status.struct_size = (uint32_t)sizeof(status);
    return status;
}

static void init_mock(struct mock_broker *mock) {
    memset(mock, 0, sizeof(*mock));
    mock->window_size = ADVC_VAAPI_DECODE_EOS_MAX_PROGRESS_OUTPUTS;
    mock->frame_surface_index = -1;
}

static void add_item(struct mock_broker *mock, int32_t result, uint64_t id,
                     uint64_t pts_ns, uint32_t flags, uint32_t kind) {
    struct mock_item *item;
    CHECK(mock->item_count < ARRAY_SIZE(mock->items));
    item = &mock->items[mock->item_count++];
    memset(item, 0, sizeof(*item));
    item->result = result;
    item->output.buffer_id = id;
    item->output.pts_ns = pts_ns;
    item->output.flags = flags;
    item->output.kind = kind;
}

static int32_t mock_try_signal(void *opaque) {
    struct mock_broker *mock = opaque;
    ++mock->signal_calls;
    if (mock->signal_result_index < mock->signal_result_count)
        return mock->signal_results[mock->signal_result_index++];
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static int32_t mock_try_dequeue(
    void *opaque, struct advc_vaapi_decode_eos_output *output) {
    struct mock_broker *mock = opaque;
    const struct mock_item *item;
    ++mock->dequeue_calls;
    if (mock->outstanding_leases >= mock->window_size)
        return ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL;
    if (mock->item_index >= mock->item_count)
        return ADVC_VAAPI_DECODE_EOS_IO_RETRY;
    item = &mock->items[mock->item_index++];
    if (item->result != ADVC_VAAPI_DECODE_EOS_IO_OK)
        return item->result;
    *output = item->output;
    ++mock->outstanding_leases;
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static int32_t mock_handle_frame(
    void *opaque, const struct advc_vaapi_decode_eos_output *output) {
    struct mock_broker *mock = opaque;
    CHECK(mock->frame_calls < ARRAY_SIZE(mock->handled_ids));
    mock->handled_ids[mock->frame_calls++] = output->buffer_id;
    if (mock->fail_frame) return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    if (mock->frame_surface_index >= 0) {
        CHECK((size_t)mock->frame_surface_index <
              ARRAY_SIZE(mock->surface_states));
        mock->surface_states[mock->frame_surface_index] = MOCK_SURFACE_READY;
    }
    if (!mock->retain_frames) {
        CHECK(mock->outstanding_leases > 0);
        --mock->outstanding_leases;
    }
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static int32_t mock_release_output(
    void *opaque, const struct advc_vaapi_decode_eos_output *output) {
    struct mock_broker *mock = opaque;
    CHECK(mock->release_calls < ARRAY_SIZE(mock->released_ids));
    mock->released_ids[mock->release_calls++] = output->buffer_id;
    if (mock->fail_release) return ADVC_VAAPI_DECODE_EOS_IO_FATAL;
    CHECK(mock->outstanding_leases > 0);
    --mock->outstanding_leases;
    return ADVC_VAAPI_DECODE_EOS_IO_OK;
}

static void mock_mark_remaining_pending_failed(void *opaque) {
    struct mock_broker *mock = opaque;
    size_t i;
    ++mock->terminal_calls;
    for (i = 0; i < ARRAY_SIZE(mock->surface_states); ++i) {
        if (mock->surface_states[i] == MOCK_SURFACE_PENDING &&
            mock->surface_sessions[i] == mock->terminal_session)
            mock->surface_states[i] = MOCK_SURFACE_ERROR;
    }
}

static struct advc_vaapi_decode_eos_ops mock_ops(struct mock_broker *mock) {
    struct advc_vaapi_decode_eos_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.opaque = mock;
    ops.try_signal = mock_try_signal;
    ops.try_dequeue = mock_try_dequeue;
    ops.handle_frame = mock_handle_frame;
    ops.release_output = mock_release_output;
    ops.mark_remaining_pending_failed =
        mock_mark_remaining_pending_failed;
    return ops;
}

static void release_retained_frame(struct mock_broker *mock) {
    CHECK(mock->outstanding_leases > 0);
    --mock->outstanding_leases;
}

static int mock_normal_drain_one(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops) {
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    int32_t result =
        advc_vaapi_decode_eos_state_progress(state, ops, 1, &status);
    if (result == ADVC_VAAPI_DECODE_EOS_RESULT_OK ||
        result == ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE)
        return status.frames_processed > 0 ? 1 : 0;
    if (result == ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK ||
        result == ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE)
        return 0;
    return -1;
}

static void test_exact_opt_in_gate(void) {
    CHECK(!advc_vaapi_decode_eos_gate_value_enabled(NULL));
    CHECK(!advc_vaapi_decode_eos_gate_value_enabled("1"));
    CHECK(!advc_vaapi_decode_eos_gate_value_enabled("validated-signal-progress-v1 "));
    CHECK(advc_vaapi_decode_eos_gate_value_enabled(
        ADVC_VAAPI_DECODE_EOS_OPT_IN_VALUE));

    CHECK(unsetenv(ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV) == 0);
    CHECK(!advc_vaapi_decode_eos_gate_enabled());
    CHECK(setenv(ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV, "validated-v1", 1) == 0);
    CHECK(!advc_vaapi_decode_eos_gate_enabled());
    CHECK(setenv(ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV,
                 ADVC_VAAPI_DECODE_EOS_OPT_IN_VALUE, 1) == 0);
    CHECK(advc_vaapi_decode_eos_gate_enabled());
    CHECK(unsetenv(ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV) == 0);
}

static void test_progress_requires_signal(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_NOT_SIGNALED);
    CHECK(mock.dequeue_calls == 0);
    CHECK(status.phase == ADVC_VAAPI_DECODE_EOS_OPEN);
}

static void test_signal_retry_and_idempotence(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    mock.signal_results[0] = ADVC_VAAPI_DECODE_EOS_IO_RETRY;
    mock.signal_results[1] = ADVC_VAAPI_DECODE_EOS_IO_OK;
    mock.signal_result_count = 2;
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 11, 100, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);

    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK);
    CHECK(state.phase == ADVC_VAAPI_DECODE_EOS_SIGNAL_PENDING);
    CHECK((status.flags & ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_REQUESTED) != 0);
    CHECK((status.flags & ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_QUEUED) == 0);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(mock.frame_calls == 1);
    CHECK((status.flags & ADVC_VAAPI_DECODE_EOS_STATUS_BUDGET_EXHAUSTED) != 0);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(state.phase == ADVC_VAAPI_DECODE_EOS_DRAINING);
    CHECK(mock.signal_calls == 2);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(mock.signal_calls == 2);
}

static void test_control_eos_skips_frame_matching(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    /* A deliberately frame-looking PTS must not cause a surface lookup. */
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 21, 777,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.frame_calls == 0);
    CHECK(mock.release_calls == 1);
    CHECK(mock.released_ids[0] == 21);
    CHECK(mock.outstanding_leases == 0);
    CHECK(status.controls_released == 1);
    CHECK((status.flags & ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_EOS_SEEN) != 0);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.dequeue_calls == 1);
}

static void test_frame_carrying_eos(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 31, 888,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) == 0);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.frame_calls == 1);
    CHECK(mock.release_calls == 0);
    CHECK(status.frames_processed == 1);
}

static void test_normal_drain_shares_terminal_state(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();

    /* A normal surface query consumes control EOS through the shared state. */
    init_mock(&mock);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 35, 999,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(mock_normal_drain_one(&state, &ops) == 0);
    CHECK(state.phase == ADVC_VAAPI_DECODE_EOS_COMPLETE);
    CHECK(state.controls_released_total == 1);
    CHECK(mock.release_calls == 1);
    CHECK(mock.dequeue_calls == 1);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.dequeue_calls == 1);

    /* A normal surface query may likewise consume a frame carrying EOS. */
    init_mock(&mock);
    status = fresh_status();
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 36, 1000,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(mock_normal_drain_one(&state, &ops) == 1);
    CHECK(state.phase == ADVC_VAAPI_DECODE_EOS_COMPLETE);
    CHECK(state.frames_processed_total == 1);
    CHECK(state.controls_released_total == 0);
    CHECK(mock.frame_calls == 1);
    CHECK(mock.dequeue_calls == 1);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.dequeue_calls == 1);
}

static void test_terminal_eos_fails_unmatched_pending_surfaces(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();

    init_mock(&mock);
    mock.terminal_session = 7;
    mock.surface_sessions[0] = 7;
    mock.surface_states[0] = MOCK_SURFACE_PENDING;
    mock.surface_sessions[1] = 7;
    mock.surface_states[1] = MOCK_SURFACE_READY;
    mock.surface_sessions[2] = 8;
    mock.surface_states[2] = MOCK_SURFACE_PENDING;
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 37, 1001,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.terminal_calls == 1);
    CHECK(mock.surface_states[0] == MOCK_SURFACE_ERROR);
    CHECK(mock.surface_states[1] == MOCK_SURFACE_READY);
    CHECK(mock.surface_states[2] == MOCK_SURFACE_PENDING);

    init_mock(&mock);
    status = fresh_status();
    mock.terminal_session = 7;
    mock.frame_surface_index = 0;
    mock.surface_sessions[0] = 7;
    mock.surface_states[0] = MOCK_SURFACE_PENDING;
    mock.surface_sessions[1] = 7;
    mock.surface_states[1] = MOCK_SURFACE_PENDING;
    mock.surface_sessions[2] = 7;
    mock.surface_states[2] = MOCK_SURFACE_READY;
    mock.surface_sessions[3] = 8;
    mock.surface_states[3] = MOCK_SURFACE_PENDING;
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 38, 1002,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.terminal_calls == 1);
    CHECK(mock.surface_states[0] == MOCK_SURFACE_READY);
    CHECK(mock.surface_states[1] == MOCK_SURFACE_ERROR);
    CHECK(mock.surface_states[2] == MOCK_SURFACE_READY);
    CHECK(mock.surface_states[3] == MOCK_SURFACE_PENDING);
}

static void test_bad_output_cleanup(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 41, 0, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) == 0);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_FAILED);
    CHECK(mock.frame_calls == 0);
    CHECK(mock.release_calls == 1);
    CHECK(mock.released_ids[0] == 41);

    init_mock(&mock);
    mock.fail_frame = 1;
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 42, 9, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) == 0);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 1, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_FAILED);
    CHECK(mock.frame_calls == 1);
    CHECK(mock.release_calls == 1);
    CHECK(mock.released_ids[0] == 42);
}

static void test_bounded_budget(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 51, 1, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 52, 2, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 53, 3, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) == 0);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 2, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_OK);
    CHECK(status.outputs_processed == 2);
    CHECK(mock.dequeue_calls == 2);
    CHECK(mock.item_index == 2);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 0, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT);
    CHECK(advc_vaapi_decode_eos_state_progress(
              &state, &ops, ADVC_VAAPI_DECODE_EOS_MAX_PROGRESS_OUTPUTS + 1,
              &status) == ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT);
    CHECK(mock.dequeue_calls == 2);
}

static void test_finite_output_window_never_spins(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    mock.window_size = 2;
    mock.retain_frames = 1;
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 61, 1, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 62, 2, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 63, 3, 0,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME);
    add_item(&mock, ADVC_VAAPI_DECODE_EOS_IO_OK, 64, 3,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS,
             ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL);
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) == 0);

    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 8, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE);
    CHECK(status.outputs_processed == 2);
    CHECK(mock.dequeue_calls == 3);
    CHECK(mock.outstanding_leases == 2);

    /* The caller can now push/release one surface before asking for progress. */
    release_retained_frame(&mock);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 8, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE);
    CHECK(status.outputs_processed == 1);
    CHECK(mock.dequeue_calls == 5);
    CHECK(mock.outstanding_leases == 2);

    /* Release retained frames; the control EOS can then occupy and free a slot. */
    release_retained_frame(&mock);
    release_retained_frame(&mock);
    status = fresh_status();
    CHECK(advc_vaapi_decode_eos_state_progress(&state, &ops, 8, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE);
    CHECK(mock.frame_calls == 3);
    CHECK(mock.release_calls == 1);
    CHECK(mock.released_ids[0] == 64);
    CHECK(mock.outstanding_leases == 0);
}

static void test_signal_window_pressure_is_distinct(void) {
    struct advc_vaapi_decode_eos_state state;
    struct mock_broker mock;
    struct advc_vaapi_decode_eos_ops ops;
    struct advc_vaapi_decode_eos_status_v1 status = fresh_status();
    init_mock(&mock);
    mock.signal_results[0] = ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL;
    mock.signal_result_count = 1;
    ops = mock_ops(&mock);
    advc_vaapi_decode_eos_state_init(&state);
    CHECK(advc_vaapi_decode_eos_state_signal(&state, &ops, &status) ==
          ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE);
    CHECK((status.flags &
           ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_WINDOW_FULL) != 0);
    CHECK(state.phase == ADVC_VAAPI_DECODE_EOS_SIGNAL_PENDING);
}

int main(void) {
    CHECK(ADVC_VAAPI_DECODE_EOS_ABI_VERSION == 0x00010000u);
    CHECK(ADVC_VAAPI_DECODE_EOS_MAX_PROGRESS_OUTPUTS == 8u);
    test_exact_opt_in_gate();
    test_progress_requires_signal();
    test_signal_retry_and_idempotence();
    test_control_eos_skips_frame_matching();
    test_frame_carrying_eos();
    test_normal_drain_shares_terminal_state();
    test_terminal_eos_fails_unmatched_pending_surfaces();
    test_bad_output_cleanup();
    test_bounded_budget();
    test_finite_output_window_never_spins();
    test_signal_window_pressure_is_distinct();
    puts("decode_eos_test: ok");
    return 0;
}
