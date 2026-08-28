#include "encode_surface.h"
#include "encode_surface_cache_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct fake_state {
    int api_level;
    int creates;
    int acquires;
    int releases;
    int submits;
    int submit_result;
    int consumed_fence;
    int closed_fence;
    int eos;
};

static void test_ahb_cache_policy(void) {
    assert(advc_egl_ahb_async_cache_allowed(1, 1, 0));
    /* Missing native-fence extensions must retain synchronous teardown. */
    assert(!advc_egl_ahb_async_cache_allowed(0, 1, 0));
    /* API < 31 has no public system-wide AHardwareBuffer identity. */
    assert(!advc_egl_ahb_async_cache_allowed(1, 0, -1));
    /* A runtime getId failure must never fall back to pointer identity. */
    assert(!advc_egl_ahb_async_cache_allowed(1, 1, -1));
}

static int fake_api(void *userdata) {
    return ((struct fake_state *)userdata)->api_level;
}
static int fake_create(void *codec, void **window, void *userdata) {
    struct fake_state *state = (struct fake_state *)userdata;
    ++state->creates;
    *window = codec;
    return 0;
}
static void fake_acquire(void *window, void *userdata) {
    (void)window;
    ++((struct fake_state *)userdata)->acquires;
}
static void fake_release(void *window, void *userdata) {
    (void)window;
    ++((struct fake_state *)userdata)->releases;
}
static int fake_submit(void *window, const struct advc_encode_ahb_frame *frame,
                       int *release_fence_fd, void *userdata) {
    struct fake_state *state = (struct fake_state *)userdata;
    (void)window;
    ++state->submits;
    state->consumed_fence = frame->acquire_fence_fd;
    *release_fence_fd = 73;
    return state->submit_result;
}
static int fake_eos(void *codec, void *userdata) {
    (void)codec;
    ++((struct fake_state *)userdata)->eos;
    return 0;
}
static void fake_close_fence(int fence_fd, void *userdata) {
    ((struct fake_state *)userdata)->closed_fence = fence_fd;
}

static struct advc_encode_surface_ops make_ops(uint32_t features) {
    struct advc_encode_surface_ops ops = {
        .features = features,
        .get_api_level = fake_api,
        .create_input_surface = fake_create,
        .acquire_window = fake_acquire,
        .release_window = fake_release,
        .signal_end_of_input_stream = fake_eos,
        .submit_ahb = fake_submit,
        .close_fence = fake_close_fence,
    };
    return ops;
}

static void test_fail_closed_negotiation(void) {
    struct fake_state state = {.api_level = 25};
    struct advc_encode_surface_ops ops = make_ops(
        ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW |
        ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY);
    struct advc_encode_surface *surface = (void *)(uintptr_t)1;
    assert(!advc_encode_surface_route_supported(
        &ops, ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, &state));
    assert(advc_encode_surface_create(
               &state, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
               ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, &ops, &state,
               &surface) == ADVC_ENCODE_SURFACE_UNSUPPORTED);
    assert(surface == NULL && state.creates == 0);

    state.api_level = 36;
    assert(advc_encode_surface_route_supported(
        &ops, ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, &state));
    assert(!advc_encode_surface_route_supported(
        &ops, ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER, &state));
    ops.features |= ADVC_ENCODE_SURFACE_FEATURE_AHB_IMPORT |
                    ADVC_ENCODE_SURFACE_FEATURE_EXPLICIT_FENCE;
    ops.submit_ahb = NULL;
    assert(!advc_encode_surface_route_supported(
        &ops, ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER, &state));
    ops.submit_ahb = fake_submit;
    ops.close_fence = NULL;
    assert(!advc_encode_surface_route_supported(
        &ops, ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER, &state));
}

static void test_direct_window_lifetime_and_eos(void) {
    struct fake_state state = {.api_level = 36};
    struct advc_encode_surface_ops ops = make_ops(
        ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW |
        ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY);
    struct advc_encode_surface *surface = NULL;
    void *window = NULL;
    assert(advc_encode_surface_create(
               &state, ADVC_ENCODE_SURFACE_CODEC_STARTED,
               ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, &ops, &state,
               &surface) == ADVC_ENCODE_SURFACE_BAD_STATE);
    assert(advc_encode_surface_create(
               &state, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
               ADVC_ENCODE_SURFACE_ROUTE_DIRECT_WINDOW, &ops, &state,
               &surface) == ADVC_ENCODE_SURFACE_OK);
    assert(state.creates == 1);
    assert(advc_encode_surface_acquire_window(surface, &window) ==
           ADVC_ENCODE_SURFACE_OK);
    assert(window == &state && state.acquires == 1);
    advc_encode_surface_release_window(surface, window);
    assert(state.releases == 1);
    assert(advc_encode_surface_signal_eos(surface) ==
           ADVC_ENCODE_SURFACE_BAD_STATE);
    assert(advc_encode_surface_mark_started(surface) == ADVC_ENCODE_SURFACE_OK);
    assert(advc_encode_surface_signal_eos(surface) == ADVC_ENCODE_SURFACE_OK);
    assert(state.eos == 1);
    assert(advc_encode_surface_signal_eos(surface) ==
           ADVC_ENCODE_SURFACE_BAD_STATE);
    assert(advc_encode_surface_acquire_window(surface, &window) ==
           ADVC_ENCODE_SURFACE_BAD_STATE);
    advc_encode_surface_destroy(surface);
    assert(state.releases == 2);
}

static void test_ahb_fence_ownership(void) {
    struct fake_state state = {.api_level = 36};
    struct advc_encode_surface_ops ops = make_ops(
        ADVC_ENCODE_SURFACE_FEATURE_CODEC_WINDOW |
        ADVC_ENCODE_SURFACE_FEATURE_AHB_IMPORT |
        ADVC_ENCODE_SURFACE_FEATURE_EXPLICIT_FENCE |
        ADVC_ENCODE_SURFACE_FEATURE_NO_CPU_COPY);
    struct advc_encode_surface *surface = NULL;
    struct advc_encode_ahb_frame frame = {
        .hardware_buffer = &state,
        .width = 1920,
        .height = 1080,
        .format = 1,
        .usage = 0x100,
        .presentation_time_ns = 1000000,
        .acquire_fence_fd = 41,
    };
    int release_fence = 99;
    assert(advc_encode_surface_create(
               &state, ADVC_ENCODE_SURFACE_CODEC_CONFIGURED,
               ADVC_ENCODE_SURFACE_ROUTE_AHB_RENDER, &ops, &state,
               &surface) == ADVC_ENCODE_SURFACE_OK);
    assert(advc_encode_surface_submit_ahb(surface, &frame, &release_fence) ==
           ADVC_ENCODE_SURFACE_BAD_STATE);
    assert(release_fence == -1 && state.submits == 0);
    assert(advc_encode_surface_mark_started(surface) == ADVC_ENCODE_SURFACE_OK);
    assert(advc_encode_surface_submit_ahb(surface, &frame, &release_fence) ==
           ADVC_ENCODE_SURFACE_OK);
    assert(state.submits == 1 && state.consumed_fence == 41);
    assert(release_fence == 73);
    state.submit_result = -1;
    frame.acquire_fence_fd = 42;
    assert(advc_encode_surface_submit_ahb(surface, &frame, &release_fence) ==
           ADVC_ENCODE_SURFACE_PLATFORM_ERROR);
    assert(state.submits == 2 && state.consumed_fence == 42);
    assert(state.closed_fence == 73 && release_fence == -1);
    state.submit_result = 0;
    assert(advc_encode_surface_signal_eos(surface) == ADVC_ENCODE_SURFACE_OK);
    assert(advc_encode_surface_submit_ahb(surface, &frame, &release_fence) ==
           ADVC_ENCODE_SURFACE_BAD_STATE);
    assert(state.submits == 2 && release_fence == -1);
    advc_encode_surface_destroy(surface);
}

int main(void) {
    test_ahb_cache_policy();
    test_fail_closed_negotiation();
    test_direct_window_lifetime_and_eos();
    test_ahb_fence_ownership();
    puts("encode_surface_test: PASS");
    return 0;
}
