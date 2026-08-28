#include "advc_vaapi_policy.h"
#include "advc_vaapi_decode_runtime.h"
#include "advc_vaapi_decode_eos_private.h"
#include "advc_vaapi_encode_runtime.h"

#include "advc/client.h"

extern int lindex_firefox_rdd_debug_enabled(void) __attribute__((weak));

#include <va/va_backend.h>

#include <errno.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADVC_VAAPI_DEFAULT_SOCKET "/run/android-drm/advc-broker-1.1.sock"

enum advc_surface_route {
    ADVC_SURFACE_ROUTE_UNKNOWN = 0,
    ADVC_SURFACE_ROUTE_DECODE = 1,
    ADVC_SURFACE_ROUTE_ENCODE = 2,
};

/*
 * vaCreateSurfaces2 has no config argument. FFmpeg's standard VAAPI path
 * creates a config (and normally queries its surface constraints) immediately
 * before it allocates a pool, but passes only MemoryType=VA and PixelFormat
 * to vaCreateSurfaces2. Keep that standard per-thread association without
 * changing libva or FFmpeg. Explicit UsageHint/PRIME2 attributes still win.
 */
static _Thread_local enum advc_surface_route surface_route_hint =
    ADVC_SURFACE_ROUTE_UNKNOWN;

struct advc_vaapi_driver {
    struct advc_vaapi_policy policy;
    struct advc_vaapi_encode_policy encode_policy;
    struct advc_vaapi_decode_runtime *decode;
    struct advc_vaapi_encode_runtime *encode;
};

static struct advc_vaapi_driver *driver_from_context(VADriverContextP ctx) {
    return ctx == NULL ? NULL : ctx->pDriverData;
}

static int exact_env(const char *name, const char *value) {
    const char *actual = getenv(name);
    return actual != NULL && strcmp(actual, value) == 0;
}

typedef int (*firefox_avc_decode_enabled_fn)(void);

static int firefox_avc_decode_enabled(void) {
    firefox_avc_decode_enabled_fn function = NULL;
    void *symbol = dlsym(RTLD_DEFAULT,
                         "lindex_firefox_rdd_avc_decode_enabled");
    if (sizeof(function) == sizeof(symbol))
        memcpy(&function, &symbol, sizeof(function));
    return function != NULL && function();
}

static uint32_t decode_ready_mask(void) {
    uint32_t mask = advc_vaapi_validation_mask(
        getenv("ADVC_VAAPI_ENABLE_AVC"),
        getenv("ADVC_VAAPI_ENABLE_HEVC"),
        getenv("ADVC_VAAPI_ENABLE_VP9"));
    if (firefox_avc_decode_enabled()) mask |= ADVC_VAAPI_CODEC_H264;
    return mask;
}

static int decode_profile_advertised(
    const struct advc_vaapi_driver *driver, VAProfile profile) {
    return driver != NULL && advc_vaapi_policy_profile_advertised(
                                 &driver->policy, profile);
}

static VAStatus unimplemented(void) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static int generic_upload_encode_enabled(
    const struct advc_vaapi_driver *driver) {
    const char *gate = getenv("ADVC_VAAPI_ENABLE_GENERIC_UPLOAD");
    return driver != NULL && driver->encode_policy.codecs != 0 &&
           gate != NULL && strcmp(gate, "validated-nv12-v1") == 0;
}

static void trace_surface_pool(const char *api, int encode,
                               const VASurfaceID *surfaces,
                               unsigned int count, VAStatus status) {
    unsigned int i;
    if (getenv("ADVC_VAAPI_TRACE") == NULL) return;
    fprintf(stderr, "advc-vaapi: %s route=%s count=%u status=%d ids=", api,
            encode ? "encode" : "decode", count, status);
    if (status == VA_STATUS_SUCCESS && surfaces != NULL) {
        for (i = 0; i < count; ++i)
            fprintf(stderr, "%s%u", i == 0 ? "" : ",", surfaces[i]);
    }
    fputc('\n', stderr);
}

static VAStatus advc_terminate(VADriverContextP ctx) {
    struct advc_vaapi_driver *driver;
    if (ctx == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    driver = ctx->pDriverData;
    if (driver != NULL) {
        advc_vaapi_encode_runtime_destroy(driver->encode);
        advc_vaapi_decode_runtime_destroy(driver->decode);
    }
    free(driver);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus advc_query_config_profiles(VADriverContextP ctx,
                                           VAProfile *profiles,
                                           int *num_profiles) {
    static const VAProfile decode_profiles[] = {
        VAProfileH264ConstrainedBaseline,
        VAProfileH264Main,
        VAProfileHEVCMain,
        VAProfileVP9Profile0,
    };
    const struct advc_vaapi_driver *driver;
    size_t i;
    int count = 0;

    if (ctx == NULL || num_profiles == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    driver = ctx->pDriverData;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (i = 0; i < sizeof(decode_profiles) / sizeof(decode_profiles[0]);
         ++i) {
        if (!decode_profile_advertised(driver, decode_profiles[i]))
            continue;
        if (profiles != NULL) profiles[count] = decode_profiles[i];
        ++count;
    }
    if ((driver->encode_policy.codecs & ADVC_VAAPI_ENCODE_H264) != 0 &&
        !decode_profile_advertised(
            driver, VAProfileH264ConstrainedBaseline)) {
        if (profiles != NULL)
            profiles[count] = VAProfileH264ConstrainedBaseline;
        ++count;
    }
    if ((driver->encode_policy.codecs & ADVC_VAAPI_ENCODE_HEVC) != 0 &&
        !decode_profile_advertised(driver, VAProfileHEVCMain)) {
        if (profiles != NULL) profiles[count] = VAProfileHEVCMain;
        ++count;
    }
    *num_profiles = count;
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr, "advc-vaapi: query-profiles count=%d\n", count);
    return VA_STATUS_SUCCESS;
}

static VAStatus advc_query_config_entrypoints(VADriverContextP ctx,
                                              VAProfile profile,
                                              VAEntrypoint *entrypoints,
                                              int *num_entrypoints) {
    const struct advc_vaapi_driver *driver;
    int count = 0;

    if (ctx == NULL || num_entrypoints == NULL)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    /*
     * GStreamer VA-API probes VAProfileNone before enumerating codec
     * profiles, even when it only wants a decoder.  ProfileNone is the
     * libva convention for video processing; this driver does not implement
     * VPP, so report an empty set without turning the compatibility probe
     * into a fatal unsupported-profile error.  Unknown real codec profiles
     * remain fail-closed below.
     */
    if (profile == VAProfileNone) {
        *num_entrypoints = 0;
        if (getenv("ADVC_VAAPI_TRACE") != NULL)
            fprintf(stderr,
                    "advc-vaapi: query-entrypoints profile=%d count=0 "
                    "status=success-vpp-empty\n",
                    profile);
        return VA_STATUS_SUCCESS;
    }
    if (decode_profile_advertised(driver, profile)) {
        if (entrypoints != NULL) entrypoints[count] = VAEntrypointVLD;
        ++count;
    }
    if (((driver->encode_policy.codecs & ADVC_VAAPI_ENCODE_H264) != 0 &&
         profile == VAProfileH264ConstrainedBaseline) ||
        ((driver->encode_policy.codecs & ADVC_VAAPI_ENCODE_HEVC) != 0 &&
         profile == VAProfileHEVCMain)) {
        if (entrypoints != NULL) entrypoints[count] = VAEntrypointEncSlice;
        ++count;
    }
    if (count != 0) {
        *num_entrypoints = count;
        if (getenv("ADVC_VAAPI_TRACE") != NULL)
            fprintf(stderr,
                    "advc-vaapi: query-entrypoints profile=%d count=%d "
                    "status=success\n",
                    profile, count);
        return VA_STATUS_SUCCESS;
    }
    *num_entrypoints = 0;
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: query-entrypoints profile=%d count=0 "
                "status=unsupported\n",
                profile);
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

static VAStatus advc_get_config_attributes(VADriverContextP ctx,
                                           VAProfile profile,
                                           VAEntrypoint entrypoint,
                                           VAConfigAttrib *attributes,
                                           int num_attributes) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    int i;

    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (attributes == NULL || num_attributes < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (entrypoint == VAEntrypointEncSlice) {
        for (i = 0; i < num_attributes; ++i) {
            uint32_t value = VA_ATTRIB_NOT_SUPPORTED;
            (void)advc_vaapi_encode_get_attribute(
                &driver->encode_policy, profile, entrypoint,
                attributes[i].type, &value);
            attributes[i].value = value;
        }
        return advc_vaapi_encode_profile_supported(
                   &driver->encode_policy, profile) &&
                       (profile == VAProfileH264ConstrainedBaseline ||
                        profile == VAProfileHEVCMain)
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (!decode_profile_advertised(driver, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (i = 0; i < num_attributes; ++i) {
        if (attributes[i].type == VAConfigAttribRTFormat)
            attributes[i].value = VA_RT_FORMAT_YUV420;
        else
            attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus advc_create_config(VADriverContextP ctx, VAProfile profile,
                                   VAEntrypoint entrypoint,
                                   VAConfigAttrib *attributes,
                                   int num_attributes, VAConfigID *config_id) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    VAStatus status;
    if (entrypoint == VAEntrypointEncSlice) {
        status = advc_vaapi_encode_create_config(
            driver->encode, profile, entrypoint, attributes, num_attributes,
            config_id);
        if (status == VA_STATUS_SUCCESS)
            surface_route_hint = ADVC_SURFACE_ROUTE_ENCODE;
        return status;
    }
    status = advc_vaapi_decode_create_config(
        driver->decode, profile, entrypoint, attributes, num_attributes,
        config_id);
    if (status == VA_STATUS_SUCCESS)
        surface_route_hint = ADVC_SURFACE_ROUTE_DECODE;
    return status;
}

static VAStatus advc_destroy_config(VADriverContextP ctx,
                                    VAConfigID config_id) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_config(config_id)) {
        status = advc_vaapi_encode_destroy_config(driver->encode, config_id);
        if (status == VA_STATUS_SUCCESS)
            surface_route_hint = ADVC_SURFACE_ROUTE_UNKNOWN;
        return status;
    }
    status = advc_vaapi_decode_destroy_config(driver->decode, config_id);
    if (status == VA_STATUS_SUCCESS)
        surface_route_hint = ADVC_SURFACE_ROUTE_UNKNOWN;
    return status;
}

static VAStatus advc_query_config_attributes(VADriverContextP ctx,
                                             VAConfigID config_id,
                                             VAProfile *profile,
                                             VAEntrypoint *entrypoint,
                                             VAConfigAttrib *attributes,
                                             int *num_attributes) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_config(config_id))
        return advc_vaapi_encode_query_config(
            driver->encode, config_id, profile, entrypoint, attributes,
            num_attributes);
    return advc_vaapi_decode_query_config(driver->decode, config_id, profile,
                                          entrypoint, attributes,
                                          num_attributes);
}

static VAStatus advc_create_surfaces(VADriverContextP ctx, int width,
                                     int height, int format, int num_surfaces,
                                     VASurfaceID *surfaces) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (width < 0 || height < 0 || num_surfaces < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VAStatus status;
    int encode = surface_route_hint == ADVC_SURFACE_ROUTE_ENCODE ||
                 (surface_route_hint == ADVC_SURFACE_ROUTE_UNKNOWN &&
                  !firefox_avc_decode_enabled() &&
                  generic_upload_encode_enabled(driver));
    surface_route_hint = ADVC_SURFACE_ROUTE_UNKNOWN;
    if (encode)
        status = advc_vaapi_encode_create_surfaces(
            driver->encode, (unsigned int)format, (unsigned int)width,
            (unsigned int)height, surfaces, (unsigned int)num_surfaces, NULL,
            0);
    else
        status = advc_vaapi_decode_create_surfaces(
            driver->decode, (unsigned int)format, (unsigned int)width,
            (unsigned int)height, surfaces, (unsigned int)num_surfaces, NULL,
            0);
    trace_surface_pool("create-surfaces", encode, surfaces,
                       (unsigned int)num_surfaces, status);
    return status;
}

static VAStatus advc_destroy_surfaces(VADriverContextP ctx,
                                      VASurfaceID *surfaces,
                                      int num_surfaces) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (num_surfaces > 0 && surfaces != NULL &&
        advc_vaapi_encode_owns_surface(surfaces[0]))
        return advc_vaapi_encode_destroy_surfaces(driver->encode, surfaces,
                                                  num_surfaces);
    return advc_vaapi_decode_destroy_surfaces(driver->decode, surfaces,
                                              num_surfaces);
}

static VAStatus advc_create_context(VADriverContextP ctx, VAConfigID config_id,
                                    int width, int height, int flag,
                                    VASurfaceID *targets, int num_targets,
                                    VAContextID *context) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_config(config_id))
        return advc_vaapi_encode_create_context(
            driver->encode, config_id, width, height, flag, targets,
            num_targets, context);
    return advc_vaapi_decode_create_context(
        driver->decode, config_id, width, height, flag, targets, num_targets,
        context);
}

static VAStatus advc_destroy_context(VADriverContextP ctx,
                                     VAContextID context) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    encode = advc_vaapi_encode_owns_context(context);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: destroy-context enter route=%s context=%u\n",
                encode ? "encode" : "decode", context);
    status = encode
                 ? advc_vaapi_encode_destroy_context(driver->encode, context)
                 : advc_vaapi_decode_destroy_context(driver->decode, context);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: destroy-context return route=%s context=%u "
                "status=%u\n",
                encode ? "encode" : "decode", context, status);
    return status;
}

static VAStatus advc_create_buffer(VADriverContextP ctx, VAContextID context,
                                   VABufferType type, unsigned int size,
                                   unsigned int num_elements, void *data,
                                   VABufferID *buffer) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_context(context))
        return advc_vaapi_encode_create_buffer(
            driver->encode, context, type, size, num_elements, data, buffer);
    return advc_vaapi_decode_create_buffer(driver->decode, context, type, size,
                                           num_elements, data, buffer);
}

static VAStatus advc_buffer_set_num_elements(VADriverContextP ctx,
                                             VABufferID buffer,
                                             unsigned int num_elements) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_buffer(buffer))
        return advc_vaapi_encode_buffer_set_num_elements(
            driver->encode, buffer, num_elements);
    return advc_vaapi_decode_buffer_set_num_elements(driver->decode, buffer,
                                                     num_elements);
}

static VAStatus advc_map_buffer(VADriverContextP ctx, VABufferID buffer,
                                void **mapped) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    int encode;
    int coded = 0;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_image_owns_buffer(
            advc_vaapi_encode_image_runtime(driver->encode), buffer))
        return advc_vaapi_image_map_buffer(
            advc_vaapi_encode_image_runtime(driver->encode), buffer, mapped);
    encode = advc_vaapi_encode_owns_buffer(buffer);
    if (encode)
        coded = advc_vaapi_encode_buffer_is_coded(driver->encode, buffer);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr, "advc-vaapi: map-buffer enter route=%s buffer=%u\n",
                encode ? "encode" : "decode", buffer);
    status = encode
                 ? advc_vaapi_encode_map_buffer(driver->encode, buffer, mapped)
                 : advc_vaapi_decode_map_buffer(driver->decode, buffer, mapped);
    if (getenv("ADVC_VAAPI_TRACE") != NULL) {
        fprintf(stderr,
                "advc-vaapi: map-buffer return route=%s buffer=%u status=%u",
                encode ? "encode" : "decode", buffer, status);
        if (coded && status == VA_STATUS_SUCCESS && mapped != NULL &&
            *mapped != NULL) {
            const VACodedBufferSegment *segment =
                (const VACodedBufferSegment *)*mapped;
            fprintf(stderr, " first_size=%u first_status=0x%x next=%d",
                    segment->size, segment->status,
                    segment->next != NULL ? 1 : 0);
        }
        fputc('\n', stderr);
    }
    return status;
}

static VAStatus advc_unmap_buffer(VADriverContextP ctx, VABufferID buffer) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_image_owns_buffer(
            advc_vaapi_encode_image_runtime(driver->encode), buffer))
        return advc_vaapi_image_unmap_buffer(
            advc_vaapi_encode_image_runtime(driver->encode), buffer);
    encode = advc_vaapi_encode_owns_buffer(buffer);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: unmap-buffer enter route=%s buffer=%u\n",
                encode ? "encode" : "decode", buffer);
    status = encode
                 ? advc_vaapi_encode_unmap_buffer(driver->encode, buffer)
                 : advc_vaapi_decode_unmap_buffer(driver->decode, buffer);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: unmap-buffer return route=%s buffer=%u status=%u\n",
                encode ? "encode" : "decode", buffer, status);
    return status;
}

static VAStatus advc_destroy_buffer(VADriverContextP ctx, VABufferID buffer) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_image_owns_buffer(
            advc_vaapi_encode_image_runtime(driver->encode), buffer))
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (advc_vaapi_encode_owns_buffer(buffer))
        return advc_vaapi_encode_destroy_buffer(driver->encode, buffer);
    return advc_vaapi_decode_destroy_buffer(driver->decode, buffer);
}

static VAStatus advc_begin_picture(VADriverContextP ctx, VAContextID context,
                                   VASurfaceID target) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    encode = advc_vaapi_encode_owns_context(context);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: begin-picture route=%s context=%u surface=%u\n",
                encode ? "encode" : "decode", context, target);
    if (encode)
        return advc_vaapi_encode_begin_picture(driver->encode, context,
                                               target);
    return advc_vaapi_decode_begin_picture(driver->decode, context, target);
}

static VAStatus advc_render_picture(VADriverContextP ctx, VAContextID context,
                                    VABufferID *buffers, int num_buffers) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_context(context))
        return advc_vaapi_encode_render_picture(
            driver->encode, context, buffers, num_buffers);
    return advc_vaapi_decode_render_picture(driver->decode, context, buffers,
                                            num_buffers);
}

static VAStatus advc_end_picture(VADriverContextP ctx, VAContextID context) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    encode = advc_vaapi_encode_owns_context(context);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr, "advc-vaapi: end-picture route=%s context=%u\n",
                encode ? "encode" : "decode", context);
    if (encode)
        return advc_vaapi_encode_end_picture(driver->encode, context);
    return advc_vaapi_decode_end_picture(driver->decode, context);
}

static VAStatus advc_sync_surface(VADriverContextP ctx, VASurfaceID surface) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    encode = advc_vaapi_encode_owns_surface(surface);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr, "advc-vaapi: sync-surface route=%s surface=%u\n",
                encode ? "encode" : "decode", surface);
    status = encode
                 ? advc_vaapi_encode_sync_surface(driver->encode, surface,
                                                   UINT64_MAX)
                 : advc_vaapi_decode_sync_surface(driver->decode, surface,
                                                   UINT64_MAX);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: sync-surface return route=%s surface=%u "
                "status=%u\n",
                encode ? "encode" : "decode", surface, status);
    return status;
}

static VAStatus advc_query_surface_status(VADriverContextP ctx,
                                          VASurfaceID surface,
                                          VASurfaceStatus *status) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_surface(surface))
        return advc_vaapi_encode_query_surface_status(driver->encode, surface,
                                                      status);
    return advc_vaapi_decode_query_surface_status(driver->decode, surface,
                                                  status);
}

static VAStatus advc_query_image_formats(VADriverContextP ctx,
                                         VAImageFormat *formats,
                                         int *num_formats) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    return advc_vaapi_image_query_formats(formats, num_formats);
}

static VAStatus advc_create_image(VADriverContextP ctx, VAImageFormat *format,
                                  int width, int height, VAImage *image) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    return advc_vaapi_image_create(
        advc_vaapi_encode_image_runtime(driver->encode), format, width,
        height, image);
}

static VAStatus advc_derive_image(VADriverContextP ctx, VASurfaceID surface,
                                  VAImage *image) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!advc_vaapi_encode_owns_surface(surface))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    return advc_vaapi_image_derive(
        advc_vaapi_encode_image_runtime(driver->encode), surface, image);
}

static VAStatus advc_destroy_image(VADriverContextP ctx, VAImageID image) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    return advc_vaapi_image_destroy(
        advc_vaapi_encode_image_runtime(driver->encode), image);
}

static VAStatus advc_set_image_palette(VADriverContextP ctx, VAImageID image,
                                       unsigned char *palette) {
    (void)ctx;
    (void)image;
    (void)palette;
    return unimplemented();
}

static VAStatus advc_get_image(VADriverContextP ctx, VASurfaceID surface,
                               int x, int y, unsigned int width,
                               unsigned int height, VAImageID image) {
    (void)ctx;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)image;
    return unimplemented();
}

static VAStatus advc_put_image(VADriverContextP ctx, VASurfaceID surface,
                               VAImageID image, int src_x, int src_y,
                               unsigned int src_width,
                               unsigned int src_height, int dst_x, int dst_y,
                               unsigned int dst_width,
                               unsigned int dst_height) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!advc_vaapi_encode_owns_surface(surface))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    return advc_vaapi_image_put(
        advc_vaapi_encode_image_runtime(driver->encode), surface, image,
        src_x, src_y, src_width, src_height, dst_x, dst_y, dst_width,
        dst_height);
}

static VAStatus advc_query_subpicture_formats(VADriverContextP ctx,
                                              VAImageFormat *formats,
                                              unsigned int *flags,
                                              unsigned int *num_formats) {
    (void)ctx;
    (void)formats;
    (void)flags;
    if (num_formats != NULL) *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus advc_create_subpicture(VADriverContextP ctx, VAImageID image,
                                       VASubpictureID *subpicture) {
    (void)ctx;
    (void)image;
    (void)subpicture;
    return unimplemented();
}

static VAStatus advc_destroy_subpicture(VADriverContextP ctx,
                                        VASubpictureID subpicture) {
    (void)ctx;
    (void)subpicture;
    return unimplemented();
}

static VAStatus advc_set_subpicture_image(VADriverContextP ctx,
                                          VASubpictureID subpicture,
                                          VAImageID image) {
    (void)ctx;
    (void)subpicture;
    (void)image;
    return unimplemented();
}

static VAStatus advc_set_subpicture_chromakey(VADriverContextP ctx,
                                              VASubpictureID subpicture,
                                              unsigned int minimum,
                                              unsigned int maximum,
                                              unsigned int mask) {
    (void)ctx;
    (void)subpicture;
    (void)minimum;
    (void)maximum;
    (void)mask;
    return unimplemented();
}

static VAStatus advc_set_subpicture_alpha(VADriverContextP ctx,
                                          VASubpictureID subpicture,
                                          float alpha) {
    (void)ctx;
    (void)subpicture;
    (void)alpha;
    return unimplemented();
}

static VAStatus advc_associate_subpicture(
    VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *surfaces,
    int num_surfaces, short src_x, short src_y, unsigned short src_width,
    unsigned short src_height, short dst_x, short dst_y,
    unsigned short dst_width, unsigned short dst_height, unsigned int flags) {
    (void)ctx;
    (void)subpicture;
    (void)surfaces;
    (void)num_surfaces;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dst_x;
    (void)dst_y;
    (void)dst_width;
    (void)dst_height;
    (void)flags;
    return unimplemented();
}

static VAStatus advc_deassociate_subpicture(VADriverContextP ctx,
                                            VASubpictureID subpicture,
                                            VASurfaceID *surfaces,
                                            int num_surfaces) {
    (void)ctx;
    (void)subpicture;
    (void)surfaces;
    (void)num_surfaces;
    return unimplemented();
}

static VAStatus advc_query_display_attributes(VADriverContextP ctx,
                                              VADisplayAttribute *attributes,
                                              int *num_attributes) {
    (void)ctx;
    (void)attributes;
    if (num_attributes != NULL) *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus advc_get_display_attributes(VADriverContextP ctx,
                                            VADisplayAttribute *attributes,
                                            int num_attributes) {
    (void)ctx;
    (void)attributes;
    (void)num_attributes;
    return unimplemented();
}

static VAStatus advc_set_display_attributes(VADriverContextP ctx,
                                            VADisplayAttribute *attributes,
                                            int num_attributes) {
    (void)ctx;
    (void)attributes;
    (void)num_attributes;
    return unimplemented();
}

static VAStatus advc_create_surfaces2(
    VADriverContextP ctx, unsigned int format, unsigned int width,
    unsigned int height, VASurfaceID *surfaces, unsigned int num_surfaces,
    VASurfaceAttrib *attributes, unsigned int num_attributes) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    int encode = surface_route_hint == ADVC_SURFACE_ROUTE_ENCODE ||
                 (surface_route_hint == ADVC_SURFACE_ROUTE_UNKNOWN &&
                  !firefox_avc_decode_enabled() &&
                  generic_upload_encode_enabled(driver));
    unsigned int i;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    for (i = 0; i < num_attributes; ++i) {
        if (attributes == NULL) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (getenv("ADVC_VAAPI_TRACE") != NULL) {
            fprintf(stderr,
                    "advc-vaapi: create-surfaces2-attr index=%u type=%u "
                    "value_type=%d value_i=%d\n",
                    i, (unsigned int)attributes[i].type,
                    (int)attributes[i].value.type,
                    attributes[i].value.type == VAGenericValueTypeInteger
                        ? attributes[i].value.value.i
                        : 0);
        }
        if (attributes[i].type == VASurfaceAttribUsageHint &&
            attributes[i].value.type == VAGenericValueTypeInteger &&
            ((uint32_t)attributes[i].value.value.i &
             VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER) != 0) {
            encode = 1;
        } else if (attributes[i].type == VASurfaceAttribUsageHint &&
                   attributes[i].value.type == VAGenericValueTypeInteger &&
                   ((uint32_t)attributes[i].value.value.i &
                    VA_SURFACE_ATTRIB_USAGE_HINT_DECODER) != 0) {
            encode = 0;
        }
        if (attributes[i].type == VASurfaceAttribMemoryType &&
            attributes[i].value.type == VAGenericValueTypeInteger &&
            (uint32_t)attributes[i].value.value.i ==
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
            encode = 1;
    }
    VAStatus status;
    surface_route_hint = ADVC_SURFACE_ROUTE_UNKNOWN;
    if (encode)
        status = advc_vaapi_encode_create_surfaces(
            driver->encode, format, width, height, surfaces, num_surfaces,
            attributes, num_attributes);
    else
        status = advc_vaapi_decode_create_surfaces(
            driver->decode, format, width, height, surfaces, num_surfaces,
            attributes, num_attributes);
    trace_surface_pool("create-surfaces2", encode, surfaces, num_surfaces,
                       status);
    return status;
}

static VAStatus advc_query_surface_attributes(
    VADriverContextP ctx, VAConfigID config_id, VASurfaceAttrib *attributes,
    unsigned int *num_attributes) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_config(config_id))
        surface_route_hint = ADVC_SURFACE_ROUTE_ENCODE;
    else
        surface_route_hint = ADVC_SURFACE_ROUTE_DECODE;
    if (advc_vaapi_encode_owns_config(config_id))
        return advc_vaapi_encode_query_surface_attributes(
            driver->encode, config_id, attributes, num_attributes);
    return advc_vaapi_decode_query_surface_attributes(
        driver->decode, config_id, attributes, num_attributes);
}

static VAStatus advc_export_surface_handle(VADriverContextP ctx,
                                           VASurfaceID surface,
                                           uint32_t mem_type, uint32_t flags,
                                           void *descriptor) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    VAStatus status;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (advc_vaapi_encode_owns_surface(surface))
        status = advc_vaapi_encode_export_surface(
            driver->encode, surface, mem_type, flags, descriptor);
    else
        status = advc_vaapi_decode_export_surface(
            driver->decode, surface, mem_type, flags, descriptor);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: export-surface route=%s surface=%u "
                "mem_type=0x%x flags=0x%x status=%d\n",
                advc_vaapi_encode_owns_surface(surface) ? "encode" : "decode",
                surface, mem_type, flags, status);
    return status;
}

static VAStatus advc_sync_surface2(VADriverContextP ctx, VASurfaceID surface,
                                   uint64_t timeout_ns) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    int encode;
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    encode = advc_vaapi_encode_owns_surface(surface);
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: sync-surface2 route=%s surface=%u timeout=%llu\n",
                encode ? "encode" : "decode", surface,
                (unsigned long long)timeout_ns);
    if (encode)
        return advc_vaapi_encode_sync_surface(driver->encode, surface,
                                              timeout_ns);
    return advc_vaapi_decode_sync_surface(driver->decode, surface, timeout_ns);
}

static VAStatus advc_sync_buffer(VADriverContextP ctx, VABufferID buffer,
                                 uint64_t timeout_ns) {
    struct advc_vaapi_driver *driver = driver_from_context(ctx);
    if (driver == NULL) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!advc_vaapi_encode_owns_buffer(buffer))
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (getenv("ADVC_VAAPI_TRACE") != NULL)
        fprintf(stderr,
                "advc-vaapi: sync-buffer buffer=%u timeout=%llu\n", buffer,
                (unsigned long long)timeout_ns);
    return advc_vaapi_encode_sync_buffer(driver->encode, buffer, timeout_ns);
}

static void install_vtable(struct VADriverVTable *vtable) {
    vtable->vaTerminate = advc_terminate;
    vtable->vaQueryConfigProfiles = advc_query_config_profiles;
    vtable->vaQueryConfigEntrypoints = advc_query_config_entrypoints;
    vtable->vaGetConfigAttributes = advc_get_config_attributes;
    vtable->vaCreateConfig = advc_create_config;
    vtable->vaDestroyConfig = advc_destroy_config;
    vtable->vaQueryConfigAttributes = advc_query_config_attributes;
    vtable->vaCreateSurfaces = advc_create_surfaces;
    vtable->vaDestroySurfaces = advc_destroy_surfaces;
    vtable->vaCreateContext = advc_create_context;
    vtable->vaDestroyContext = advc_destroy_context;
    vtable->vaCreateBuffer = advc_create_buffer;
    vtable->vaBufferSetNumElements = advc_buffer_set_num_elements;
    vtable->vaMapBuffer = advc_map_buffer;
    vtable->vaUnmapBuffer = advc_unmap_buffer;
    vtable->vaDestroyBuffer = advc_destroy_buffer;
    vtable->vaBeginPicture = advc_begin_picture;
    vtable->vaRenderPicture = advc_render_picture;
    vtable->vaEndPicture = advc_end_picture;
    vtable->vaSyncSurface = advc_sync_surface;
    vtable->vaQuerySurfaceStatus = advc_query_surface_status;
    vtable->vaQueryImageFormats = advc_query_image_formats;
    vtable->vaCreateImage = advc_create_image;
    vtable->vaDeriveImage = advc_derive_image;
    vtable->vaDestroyImage = advc_destroy_image;
    vtable->vaSetImagePalette = advc_set_image_palette;
    vtable->vaGetImage = advc_get_image;
    vtable->vaPutImage = advc_put_image;
    vtable->vaQuerySubpictureFormats = advc_query_subpicture_formats;
    vtable->vaCreateSubpicture = advc_create_subpicture;
    vtable->vaDestroySubpicture = advc_destroy_subpicture;
    vtable->vaSetSubpictureImage = advc_set_subpicture_image;
    vtable->vaSetSubpictureChromakey = advc_set_subpicture_chromakey;
    vtable->vaSetSubpictureGlobalAlpha = advc_set_subpicture_alpha;
    vtable->vaAssociateSubpicture = advc_associate_subpicture;
    vtable->vaDeassociateSubpicture = advc_deassociate_subpicture;
    vtable->vaQueryDisplayAttributes = advc_query_display_attributes;
    vtable->vaGetDisplayAttributes = advc_get_display_attributes;
    vtable->vaSetDisplayAttributes = advc_set_display_attributes;
    vtable->vaCreateSurfaces2 = advc_create_surfaces2;
    vtable->vaQuerySurfaceAttributes = advc_query_surface_attributes;
    vtable->vaExportSurfaceHandle = advc_export_surface_handle;
    vtable->vaSyncSurface2 = advc_sync_surface2;
    vtable->vaSyncBuffer = advc_sync_buffer;
}

static VADriverContextP driver_context_from_display(VADisplay display) {
    VADisplayContextP display_context = (VADisplayContextP)display;
    if (display_context == NULL ||
        display_context->vadpy_magic != VA_DISPLAY_MAGIC ||
        display_context->pDriverContext == NULL)
        return NULL;
    return display_context->pDriverContext;
}

static int32_t private_eos_invalid_display_status(
    struct advc_vaapi_decode_eos_status_v1 *status) {
    uint32_t caller_size;
    if (status == NULL || status->struct_size < sizeof(*status))
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    caller_size = status->struct_size;
    memset(status, 0, sizeof(*status));
    status->struct_size = caller_size;
    status->abi_version = ADVC_VAAPI_DECODE_EOS_ABI_VERSION;
    status->phase = ADVC_VAAPI_DECODE_EOS_FAILED;
    status->flags = ADVC_VAAPI_DECODE_EOS_STATUS_FAILED;
    status->last_result = ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT;
    return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_CONTEXT;
}

static int32_t private_decode_eos_signal(
    VADisplay display, VAContextID context_id, uint32_t flags,
    struct advc_vaapi_decode_eos_status_v1 *status) {
    VADriverContextP driver_context;
    struct advc_vaapi_driver *driver;
    if (flags != 0)
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    driver_context = driver_context_from_display(display);
    driver = driver_from_context(driver_context);
    if (driver == NULL) return private_eos_invalid_display_status(status);
    return advc_vaapi_decode_signal_eos_private(driver->decode, context_id,
                                                status);
}

static int32_t private_decode_eos_progress(
    VADisplay display, VAContextID context_id, uint32_t max_outputs,
    uint32_t flags, struct advc_vaapi_decode_eos_status_v1 *status) {
    VADriverContextP driver_context;
    struct advc_vaapi_driver *driver;
    if (flags != 0)
        return ADVC_VAAPI_DECODE_EOS_RESULT_INVALID_ARGUMENT;
    driver_context = driver_context_from_display(display);
    driver = driver_from_context(driver_context);
    if (driver == NULL) return private_eos_invalid_display_status(status);
    return advc_vaapi_decode_progress_eos_private(
        driver->decode, context_id, max_outputs, status);
}

static const struct advc_vaapi_decode_eos_interface_v1
    private_decode_eos_interface = {
        .struct_size = sizeof(private_decode_eos_interface),
        .abi_version = ADVC_VAAPI_DECODE_EOS_ABI_VERSION,
        .capabilities =
            ADVC_VAAPI_DECODE_EOS_CAP_SIGNAL_PROGRESS_SPLIT |
            ADVC_VAAPI_DECODE_EOS_CAP_CONTROL_EOS_NO_PTS_MATCH |
            ADVC_VAAPI_DECODE_EOS_CAP_BOUNDED_PROGRESS,
        .signal = private_decode_eos_signal,
        .progress = private_decode_eos_progress,
    };

/*
 * vaGetLibFunc(display, ADVC_VAAPI_DECODE_EOS_GET_INTERFACE_SYMBOL) resolves
 * this symbol.  Merely finding the symbol is not an opt-in: the getter returns
 * NULL unless the exact validation gate is present.
 */
__attribute__((visibility("default")))
const struct advc_vaapi_decode_eos_interface_v1 *
advcVaGetDecodeEosInterface_1_0(void) {
    return advc_vaapi_decode_eos_gate_enabled()
               ? &private_decode_eos_interface
               : NULL;
}

/*
 * Libva's stable loader symbol. Profile advertisement remains opt-in until
 * the real-device AVC/PRIME
 * validation gate is completed. This prevents a partially validated driver
 * from displacing an application's software decoder.
 */
__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    static const char vendor[] =
        "LinDeX ADVC VA-API decode + AVC/HEVC EncSlice/PRIME2 "
        "(strict subset; gated)";
    struct advc_vaapi_driver *driver;
    struct advc_capability_set caps;
    const char *socket_path;
    uint64_t features = 0;
    uint32_t ready_codec_mask;
    uint32_t max_payload = 0;
    int fd;
    int trace = exact_env("ADVC_VAAPI_TRACE", "1") ||
                access("/run/android-drm/lindex-vaapi-trace", F_OK) == 0 ||
                (lindex_firefox_rdd_debug_enabled != NULL &&
                 lindex_firefox_rdd_debug_enabled());

    if (ctx == NULL || ctx->vtable == NULL) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init invalid-context ctx=%p vtable=%p\n",
                    (void *)ctx, ctx == NULL ? NULL : (void *)ctx->vtable);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    socket_path = getenv("ADVC_VAAPI_SOCKET");
    if (socket_path == NULL || socket_path[0] == '\0')
        socket_path = ADVC_VAAPI_DEFAULT_SOCKET;

    fd = advc_client_connect_bounded(
        socket_path, ADVC_CLIENT_CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        if (exact_env("ADVC_VAAPI_TRACE", "1"))
            fprintf(stderr,
                    "advc-vaapi: driver-init connect path=%s failed errno=%d\n",
                    socket_path, errno);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (advc_client_hello(fd,
                          ADVC_FEATURE_DECODE | ADVC_FEATURE_DECODE_PRIME |
                              ADVC_FEATURE_ENCODE | ADVC_FEATURE_DMABUF |
                              ADVC_FEATURE_NATIVE_FENCE |
                              ADVC_FEATURE_DMABUF_EGL |
                              ADVC_FEATURE_DMABUF_VULKAN,
                          &features, &max_payload) < 0 ||
        max_payload < ADVC_TRANSFER_PRIME_REPLY_SIZE ||
        advc_client_query_capabilities(fd, &caps) < 0) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init handshake failed errno=%d "
                    "features=%llu max-payload=%u\n",
                    errno, (unsigned long long)features, max_payload);
        close(fd);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    close(fd);
    /* HELLO is the transport authority. Android's codec-capability provider
     * may predate a newly enabled broker transport bit (notably 1.6 PRIME).
     * Codec presence/direction/acceleration still comes only from CAPS. */
    caps.transport_features = features;

    driver = calloc(1, sizeof(*driver));
    if (driver == NULL) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ready_codec_mask = decode_ready_mask();
    if (advc_vaapi_policy_from_capabilities(
            &caps, ready_codec_mask, &driver->policy) < 0) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init decode-policy failed errno=%d\n",
                    errno);
        free(driver);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (trace)
        fprintf(stderr,
                "advc-vaapi: decode-policy features=%llu ready=0x%x "
                "broker=0x%x advertised=0x%x\n",
                (unsigned long long)features, ready_codec_mask,
                driver->policy.broker_codecs,
                driver->policy.advertised_codecs);
    if (advc_vaapi_encode_policy_from_capabilities(
            &caps, &driver->encode_policy) < 0) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init encode-policy failed errno=%d\n",
                    errno);
        free(driver);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (getenv("ADVC_VAAPI_ENABLE_ENCODE") == NULL ||
        strcmp(getenv("ADVC_VAAPI_ENABLE_ENCODE"),
               "validated-avc-hevc-v1") != 0)
        memset(&driver->encode_policy, 0, sizeof(driver->encode_policy));
    driver->decode =
        advc_vaapi_decode_runtime_create(socket_path, &driver->policy);
    if (driver->decode == NULL) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init decode-runtime failed errno=%d\n",
                    errno);
        free(driver);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    driver->encode = advc_vaapi_encode_runtime_create(
        socket_path, &driver->encode_policy);
    if (driver->encode == NULL) {
        if (trace)
            fprintf(stderr,
                    "advc-vaapi: driver-init encode-runtime failed errno=%d\n",
                    errno);
        advc_vaapi_decode_runtime_destroy(driver->decode);
        free(driver);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    ctx->pDriverData = driver;
    ctx->version_major = 1;
    ctx->version_minor = 0;
    ctx->max_profiles = 4;
    ctx->max_entrypoints = 2;
    ctx->max_attributes = 7;
    ctx->max_image_formats = ADVC_VAAPI_IMAGE_FORMAT_COUNT;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = vendor;
    install_vtable(ctx->vtable);
    return VA_STATUS_SUCCESS;
}
