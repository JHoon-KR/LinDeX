// SPDX-License-Identifier: MIT

#include <stdarg.h>

#define DRM_LEASE_BROKER_NO_MAIN
#define ioctl broker_fixture_ioctl
#include "../../src/broker/drm_lease_broker.c"
#undef ioctl

enum fixture_scenario {
    FIXTURE_HAPPY,
    FIXTURE_DISCONNECTED,
    FIXTURE_AMBIGUOUS,
    FIXTURE_NO_MODES,
    FIXTURE_NO_ENCODER,
    FIXTURE_INVALID_CRTC_MODE,
    FIXTURE_ACTIVE_PLANE,
    FIXTURE_NO_XR24,
    FIXTURE_CONNECTOR_QUERY_FAILURE,
};

static enum fixture_scenario scenario;

int broker_fixture_ioctl(int fd, unsigned long request, ...)
{
    void *argument;
    va_list arguments;
    (void)fd;
    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);

    if (request == DRM_IOCTL_MODE_GETCONNECTOR) {
        struct drm_mode_get_connector *connector = argument;
        uint64_t encoder_pointer = connector->encoders_ptr;
        uint32_t encoder_capacity = connector->count_encoders;
        uint32_t connector_id = connector->connector_id;

        if (scenario == FIXTURE_CONNECTOR_QUERY_FAILURE && connector_id == 101) {
            errno = EIO;
            return -1;
        }
        connector->connector_type =
            (connector_id == 100 || scenario == FIXTURE_AMBIGUOUS) ?
            BROKER_DRM_MODE_CONNECTOR_DISPLAYPORT : 16U;
        connector->connector_type_id = connector_id == 100 ? 1U : 2U;
        connector->connection =
            scenario == FIXTURE_DISCONNECTED && connector_id == 100 ? 2U :
            DRM_MODE_CONNECTED;
        connector->count_modes =
            scenario == FIXTURE_NO_MODES && connector_id == 100 ? 0U : 1U;
        connector->count_encoders = 1;
        connector->encoder_id =
            scenario == FIXTURE_NO_ENCODER && connector_id == 100 ? 0U :
            (connector_id == 100 ? 200U : 201U);
        if (encoder_pointer && encoder_capacity) {
            uint32_t *encoder_ids = (uint32_t *)(uintptr_t)encoder_pointer;
            encoder_ids[0] = connector_id == 100 ? 200U : 201U;
        }
        return 0;
    }
    if (request == DRM_IOCTL_MODE_GETENCODER) {
        struct drm_mode_get_encoder *encoder = argument;
        encoder->crtc_id = 300;
        encoder->possible_crtcs = 1;
        return 0;
    }
    if (request == DRM_IOCTL_MODE_GETCRTC) {
        struct drm_mode_crtc *crtc = argument;
        crtc->mode_valid = scenario == FIXTURE_INVALID_CRTC_MODE ? 0U : 1U;
        crtc->mode.clock = 148500;
        crtc->mode.hdisplay = 1920;
        crtc->mode.htotal = 2200;
        crtc->mode.vdisplay = 1080;
        crtc->mode.vtotal = 1125;
        crtc->mode.vrefresh = 60;
        snprintf(crtc->mode.name, sizeof(crtc->mode.name), "1920x1080");
        return 0;
    }
    if (request == DRM_IOCTL_MODE_GETPLANE) {
        struct drm_mode_get_plane *plane = argument;
        uint64_t format_pointer = plane->format_type_ptr;
        uint32_t format_capacity = plane->count_format_types;
        plane->crtc_id = scenario == FIXTURE_ACTIVE_PLANE ? 300U : 0U;
        plane->fb_id = scenario == FIXTURE_ACTIVE_PLANE ? 500U : 0U;
        plane->possible_crtcs = 1;
        plane->count_format_types = 1;
        if (format_pointer && format_capacity) {
            uint32_t *formats = (uint32_t *)(uintptr_t)format_pointer;
            formats[0] = scenario == FIXTURE_NO_XR24 ? 0U :
                         AH_DRM_FORMAT_XRGB8888;
        }
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static void make_resources(struct resources *resources)
{
    static uint32_t connectors[] = {100, 101};
    static uint32_t encoders[] = {200, 201};
    static uint32_t crtcs[] = {300};
    static uint32_t planes[] = {400};
    memset(resources, 0, sizeof(*resources));
    resources->connectors = connectors;
    resources->res.count_connectors = 2;
    resources->encoders = encoders;
    resources->res.count_encoders = 2;
    resources->crtcs = crtcs;
    resources->res.count_crtcs = 1;
    resources->planes = planes;
    resources->count_planes = 1;
}

static int expect_result(enum fixture_scenario selected, int expected)
{
    struct handoff_options options;
    struct resources resources;
    uint32_t connector = 0, crtc = 0;
    int result;
    scenario = selected;
    memset(&options, 0, sizeof(options));
    make_resources(&resources);
    result = broker_auto_select_objects(7, &resources, &options,
                                        &connector, &crtc);
    if (result != expected)
        return -1;
    if (expected == 0)
        return options.object_count == 3 && options.objects[0] == 100 &&
               options.objects[1] == 300 && options.objects[2] == 400 &&
               connector == 100 && crtc == 300 ? 0 : -1;
    return options.object_count == 0 && connector == 0 && crtc == 0 ? 0 : -1;
}

int main(void)
{
    static const enum fixture_scenario failures[] = {
        FIXTURE_DISCONNECTED,
        FIXTURE_AMBIGUOUS,
        FIXTURE_NO_MODES,
        FIXTURE_NO_ENCODER,
        FIXTURE_INVALID_CRTC_MODE,
        FIXTURE_ACTIVE_PLANE,
        FIXTURE_NO_XR24,
        FIXTURE_CONNECTOR_QUERY_FAILURE,
    };
    if (expect_result(FIXTURE_HAPPY, 0) != 0)
        return 1;
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index)
        if (expect_result(failures[index], -1) != 0)
            return 1;
    puts("drm_lease_broker --auto fixture: PASS");
    return 0;
}
