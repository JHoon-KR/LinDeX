#include "advc_vaapi_decode_eos.h"

#include <stdlib.h>
#include <string.h>

static uint32_t persistent_status_flags(
    const struct advc_vaapi_decode_eos_state *state) {
    uint32_t flags = 0;
    if (state->signal_requested != 0)
        flags |= ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_REQUESTED;
    if (state->signal_queued != 0)
        flags |= ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_QUEUED;
    if (state->output_eos_seen != 0)
        flags |= ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_EOS_SEEN;
    if (state->phase == ADVC_VAAPI_DECODE_EOS_FAILED)
        flags |= ADVC_VAAPI_DECODE_EOS_STATUS_FAILED;
    return flags;
}

static void write_status(
    const struct advc_vaapi_decode_eos_state *state,
    struct advc_vaapi_decode_eos_status_v1 *status, int32_t result,
    uint32_t transient_flags, uint32_t outputs, uint32_t frames,
    uint32_t controls) {
    uint32_t caller_size;
    if (status == NULL) return;
    caller_size = status->struct_size;
    memset(status, 0, sizeof(*status));
    status->struct_size = caller_size;
    status->abi_version = ADVC_VAAPI_DECODE_EOS_ABI_VERSION;
    status->phase = state == NULL ? ADVC_VAAPI_DECODE_EOS_FAILED : state->phase;
    status->flags = transient_flags;
    if (state != NULL) {
        status->flags |= persistent_status_flags(state);
        status->outputs_processed_total = state->outputs_processed_total;
        status->frames_processed_total = state->frames_processed_total;
        status->controls_released_total = state->controls_released_total;
    }
    status->last_result = result;
    status->outputs_processed = outputs;
    status->frames_processed = frames;
    status->controls_released = controls;
}

static int status_is_valid(
    const struct advc_vaapi_decode_eos_status_v1 *status) {
    return status != NULL && status->struct_size >= sizeof(*status);
}

static int ops_are_valid(const struct advc_vaapi_decode_eos_ops *ops) {
    return ops != NULL && ops->try_signal != NULL &&
           ops->try_dequeue != NULL && ops->handle_frame != NULL &&
           ops->release_output != NULL &&
           ops->mark_remaining_pending_failed != NULL;
}

static int32_t fail_state(
    struct advc_vaapi_decode_eos_state *state,
    struct advc_vaapi_decode_eos_status_v1 *status, uint32_t outputs,
    uint32_t frames, uint32_t controls) {
    state->phase = ADVC_VAAPI_DECODE_EOS_FAILED;
    write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_FAILED, 0,
                 outputs, frames, controls);
    return ADVC_VAAPI_DECODE_EOS_RESULT_FAILED;
}

static int32_t discard_and_fail(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops,
    const struct advc_vaapi_decode_eos_output *output,
    struct advc_vaapi_decode_eos_status_v1 *status, uint32_t outputs,
    uint32_t frames, uint32_t controls) {
    if (output->buffer_id != 0)
        (void)ops->release_output(ops->opaque, output);
    return fail_state(state, status, outputs, frames, controls);
}

int advc_vaapi_decode_eos_gate_value_enabled(const char *value) {
    return value != NULL &&
           strcmp(value, ADVC_VAAPI_DECODE_EOS_OPT_IN_VALUE) == 0;
}

int advc_vaapi_decode_eos_gate_enabled(void) {
    return advc_vaapi_decode_eos_gate_value_enabled(
        getenv(ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV));
}

void advc_vaapi_decode_eos_state_init(
    struct advc_vaapi_decode_eos_state *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->phase = ADVC_VAAPI_DECODE_EOS_OPEN;
}

int32_t advc_vaapi_decode_eos_state_signal(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    int32_t io_result;
    if (state == NULL || !ops_are_valid(ops) || !status_is_valid(status)) {
        if (status != NULL)
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT, 0, 0,
                         0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_FAILED) {
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_FAILED, 0, 0,
                     0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_FAILED;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_COMPLETE) {
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE, 0,
                     0, 0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_DRAINING) {
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_OK, 0, 0, 0,
                     0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_OK;
    }

    state->signal_requested = 1;
    state->phase = ADVC_VAAPI_DECODE_EOS_SIGNAL_PENDING;
    io_result = ops->try_signal(ops->opaque);
    if (io_result == ADVC_VAAPI_DECODE_EOS_IO_OK) {
        state->signal_queued = 1;
        state->phase = ADVC_VAAPI_DECODE_EOS_DRAINING;
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_OK, 0, 0, 0,
                     0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_OK;
    }
    if (io_result == ADVC_VAAPI_DECODE_EOS_IO_RETRY ||
        io_result == ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL) {
        uint32_t flag = io_result == ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL
                            ? ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_WINDOW_FULL
                            : 0;
        int32_t result =
            io_result == ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL
                ? ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE
                : ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK;
        write_status(state, status, result, flag, 0, 0, 0);
        return result;
    }
    return fail_state(state, status, 0, 0, 0);
}

int32_t advc_vaapi_decode_eos_state_progress(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops, uint32_t max_outputs,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    uint32_t outputs = 0;
    uint32_t frames = 0;
    uint32_t controls = 0;
    if (state == NULL || !ops_are_valid(ops) || !status_is_valid(status) ||
        max_outputs == 0 ||
        max_outputs > ADVC_VAAPI_DECODE_EOS_MAX_PROGRESS_OUTPUTS) {
        if (status != NULL)
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT, 0, 0,
                         0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_OPEN) {
        write_status(state, status,
                     ADVC_VAAPI_DECODE_EOS_RESULT_NOT_SIGNALED, 0, 0, 0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_NOT_SIGNALED;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_FAILED) {
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_FAILED, 0, 0,
                     0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_FAILED;
    }
    if (state->phase == ADVC_VAAPI_DECODE_EOS_COMPLETE) {
        write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE, 0,
                     0, 0, 0);
        return ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE;
    }

    while (outputs < max_outputs) {
        struct advc_vaapi_decode_eos_output output;
        int32_t io_result;
        memset(&output, 0, sizeof(output));
        io_result = ops->try_dequeue(ops->opaque, &output);
        if (io_result == ADVC_VAAPI_DECODE_EOS_IO_RETRY) {
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK,
                         ADVC_VAAPI_DECODE_EOS_STATUS_BROKER_EMPTY, outputs,
                         frames, controls);
            return ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK;
        }
        if (io_result == ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL) {
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE,
                         ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_WINDOW_FULL,
                         outputs, frames, controls);
            return ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE;
        }
        if (io_result != ADVC_VAAPI_DECODE_EOS_IO_OK)
            return fail_state(state, status, outputs, frames, controls);

        ++outputs;
        ++state->outputs_processed_total;
        if (output.buffer_id == 0)
            return fail_state(state, status, outputs, frames, controls);

        if (output.kind == ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL) {
            if (output.flags != ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS)
                return discard_and_fail(state, ops, &output, status, outputs,
                                        frames, controls);
            if (ops->release_output(ops->opaque, &output) !=
                ADVC_VAAPI_DECODE_EOS_IO_OK)
                return fail_state(state, status, outputs, frames, controls);
            ++controls;
            ++state->controls_released_total;
            if (state->phase != ADVC_VAAPI_DECODE_EOS_DRAINING)
                return fail_state(state, status, outputs, frames, controls);
            ops->mark_remaining_pending_failed(ops->opaque);
            state->output_eos_seen = 1;
            state->phase = ADVC_VAAPI_DECODE_EOS_COMPLETE;
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE, 0, outputs,
                         frames, controls);
            return ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE;
        }

        if (output.kind != ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME)
            return discard_and_fail(state, ops, &output, status, outputs,
                                    frames, controls);
        if ((output.flags & ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS) != 0 &&
            state->phase != ADVC_VAAPI_DECODE_EOS_DRAINING)
            return discard_and_fail(state, ops, &output, status, outputs,
                                    frames, controls);
        if (ops->handle_frame(ops->opaque, &output) !=
            ADVC_VAAPI_DECODE_EOS_IO_OK)
            return discard_and_fail(state, ops, &output, status, outputs,
                                    frames, controls);
        ++frames;
        ++state->frames_processed_total;
        if ((output.flags & ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS) != 0) {
            ops->mark_remaining_pending_failed(ops->opaque);
            state->output_eos_seen = 1;
            state->phase = ADVC_VAAPI_DECODE_EOS_COMPLETE;
            write_status(state, status,
                         ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE, 0, outputs,
                         frames, controls);
            return ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE;
        }
    }

    write_status(state, status, ADVC_VAAPI_DECODE_EOS_RESULT_OK,
                 ADVC_VAAPI_DECODE_EOS_STATUS_BUDGET_EXHAUSTED, outputs,
                 frames, controls);
    return ADVC_VAAPI_DECODE_EOS_RESULT_OK;
}
