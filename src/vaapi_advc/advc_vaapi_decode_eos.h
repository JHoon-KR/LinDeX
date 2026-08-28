#ifndef ADVC_VAAPI_DECODE_EOS_H
#define ADVC_VAAPI_DECODE_EOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Private, opt-in ABI.  This is deliberately not a VA-API extension and is
 * obtained only by an integration which calls vaGetLibFunc() with the exact
 * symbol below.  A stock VA-API client neither discovers nor invokes it.
 */
#define ADVC_VAAPI_DECODE_EOS_GET_INTERFACE_SYMBOL \
    "advcVaGetDecodeEosInterface_1_0"
#define ADVC_VAAPI_DECODE_EOS_OPT_IN_ENV \
    "ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS"
#define ADVC_VAAPI_DECODE_EOS_OPT_IN_VALUE \
    "validated-signal-progress-v1"

#define ADVC_VAAPI_DECODE_EOS_ABI_MAJOR 1u
#define ADVC_VAAPI_DECODE_EOS_ABI_MINOR 0u
#define ADVC_VAAPI_DECODE_EOS_ABI_VERSION \
    ((ADVC_VAAPI_DECODE_EOS_ABI_MAJOR << 16) | \
     ADVC_VAAPI_DECODE_EOS_ABI_MINOR)

/* The broker has a finite eight-output lease window. */
#define ADVC_VAAPI_DECODE_EOS_MAX_PROGRESS_OUTPUTS 8u

/* Kept independent of protocol.h; integration must static-assert equality. */
#define ADVC_VAAPI_DECODE_EOS_OUTPUT_FLAG_EOS (1u << 0)

enum advc_vaapi_decode_eos_phase {
    ADVC_VAAPI_DECODE_EOS_OPEN = 0,
    ADVC_VAAPI_DECODE_EOS_SIGNAL_PENDING = 1,
    ADVC_VAAPI_DECODE_EOS_DRAINING = 2,
    ADVC_VAAPI_DECODE_EOS_COMPLETE = 3,
    ADVC_VAAPI_DECODE_EOS_FAILED = 4,
};

/* Stable private-ABI result values returned by signal() and progress(). */
enum advc_vaapi_decode_eos_result {
    ADVC_VAAPI_DECODE_EOS_RESULT_OK = 0,
    ADVC_VAAPI_DECODE_EOS_RESULT_WOULD_BLOCK = 1,
    ADVC_VAAPI_DECODE_EOS_RESULT_NEED_OUTPUT_RELEASE = 2,
    ADVC_VAAPI_DECODE_EOS_RESULT_COMPLETE = 3,
    ADVC_VAAPI_DECODE_EOS_RESULT_NOT_SIGNALED = 4,
    ADVC_VAAPI_DECODE_EOS_RESULT_DISABLED = 5,
    ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT = -1,
    ADVC_VAAPI_DECODE_EOS_RESULT_FAILED = -2,
    ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT = -3,
};

enum advc_vaapi_decode_eos_status_flag {
    ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_REQUESTED = 1u << 0,
    ADVC_VAAPI_DECODE_EOS_STATUS_SIGNAL_QUEUED = 1u << 1,
    ADVC_VAAPI_DECODE_EOS_STATUS_BROKER_EMPTY = 1u << 2,
    ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_WINDOW_FULL = 1u << 3,
    ADVC_VAAPI_DECODE_EOS_STATUS_BUDGET_EXHAUSTED = 1u << 4,
    ADVC_VAAPI_DECODE_EOS_STATUS_OUTPUT_EOS_SEEN = 1u << 5,
    ADVC_VAAPI_DECODE_EOS_STATUS_FAILED = 1u << 6,
};

struct advc_vaapi_decode_eos_status_v1 {
    /* Caller sets this to sizeof(struct advc_vaapi_decode_eos_status_v1). */
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t phase;
    uint32_t flags;
    int32_t last_result;
    uint32_t outputs_processed;
    uint32_t frames_processed;
    uint32_t controls_released;
    uint64_t outputs_processed_total;
    uint64_t frames_processed_total;
    uint64_t controls_released_total;
    uint32_t reserved[8];
};

/*
 * Internal nonblocking state-machine adapter.
 *
 * A successful dequeue transfers one broker output lease to this function.
 * handle_frame() consumes that lease on OK, either by releasing it or by
 * attaching it to a VA surface.  On any other return it must not consume the
 * lease; release_output() is then attempted exactly once.  A control output is
 * never passed to handle_frame(), so no PTS-to-surface lookup occurs for the
 * terminal zero-byte EOS token.  mark_remaining_pending_failed() runs exactly
 * once after the terminal output is consumed; a runtime uses it to fail closed
 * any same-session surfaces for which no output can now arrive.
 *
 * All callbacks are try/bounded operations and must not wait for an output or
 * a surface release.  Calls are externally serialized by the decode runtime.
 */
enum advc_vaapi_decode_eos_io_result {
    ADVC_VAAPI_DECODE_EOS_IO_OK = 0,
    ADVC_VAAPI_DECODE_EOS_IO_RETRY = 1,
    ADVC_VAAPI_DECODE_EOS_IO_OUTPUT_WINDOW_FULL = 2,
    ADVC_VAAPI_DECODE_EOS_IO_FATAL = -1,
};

enum advc_vaapi_decode_eos_output_kind {
    ADVC_VAAPI_DECODE_EOS_OUTPUT_FRAME = 1,
    ADVC_VAAPI_DECODE_EOS_OUTPUT_CONTROL = 2,
};

struct advc_vaapi_decode_eos_output {
    uint64_t buffer_id;
    uint64_t pts_ns;
    uint32_t flags;
    uint32_t kind;
    void *private_data;
};

struct advc_vaapi_decode_eos_ops {
    void *opaque;
    int32_t (*try_signal)(void *opaque);
    int32_t (*try_dequeue)(
        void *opaque, struct advc_vaapi_decode_eos_output *output);
    int32_t (*handle_frame)(
        void *opaque, const struct advc_vaapi_decode_eos_output *output);
    int32_t (*release_output)(
        void *opaque, const struct advc_vaapi_decode_eos_output *output);
    void (*mark_remaining_pending_failed)(void *opaque);
};

struct advc_vaapi_decode_eos_state {
    uint32_t phase;
    uint32_t signal_requested;
    uint32_t signal_queued;
    uint32_t output_eos_seen;
    uint64_t outputs_processed_total;
    uint64_t frames_processed_total;
    uint64_t controls_released_total;
};

int advc_vaapi_decode_eos_gate_value_enabled(const char *value);
int advc_vaapi_decode_eos_gate_enabled(void);

void advc_vaapi_decode_eos_state_init(
    struct advc_vaapi_decode_eos_state *state);
int32_t advc_vaapi_decode_eos_state_signal(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops,
    struct advc_vaapi_decode_eos_status_v1 *status);
int32_t advc_vaapi_decode_eos_state_progress(
    struct advc_vaapi_decode_eos_state *state,
    const struct advc_vaapi_decode_eos_ops *ops, uint32_t max_outputs,
    struct advc_vaapi_decode_eos_status_v1 *status);

#ifdef __cplusplus
}
#endif

#endif
