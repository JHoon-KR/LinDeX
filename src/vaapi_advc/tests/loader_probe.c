#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    struct VADriverContext context;
    struct VADriverVTable vtable;
    VADriverInit init;
    VAProfile profiles[8];
    VAImageFormat image_formats[4];
    VAStatus status;
    int count = -1;
    int image_count = -1;
    int i;
    void *library;

    if (argc != 2) {
        fprintf(stderr, "usage: %s advc_drv_video.so\n", argv[0]);
        return 2;
    }
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    init = (VADriverInit)dlsym(library, "__vaDriverInit_1_0");
    if (init == NULL) {
        fprintf(stderr, "driver init symbol missing: %s\n", dlerror());
        dlclose(library);
        return 1;
    }

    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;
    status = init(&context);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "driver init failed: 0x%08x\n", status);
        dlclose(library);
        return 1;
    }
    if (context.vtable->vaSyncBuffer == NULL) {
        fprintf(stderr, "vaSyncBuffer vtable entry missing\n");
        goto fail;
    }
    status = context.vtable->vaSyncBuffer(
        &context, VA_INVALID_ID, UINT64_C(0));
    if (status != VA_STATUS_ERROR_INVALID_BUFFER) {
        fprintf(stderr,
                "vaSyncBuffer FFmpeg probe contract failed: status=0x%08x\n",
                status);
        goto fail;
    }
    status = context.vtable->vaQueryConfigProfiles(&context, profiles, &count);
    if (status != VA_STATUS_SUCCESS || count < 0 || count > 3 ||
        context.str_vendor == NULL || context.max_image_formats != 2) {
        fprintf(stderr,
                "fail-closed query failed: status=0x%08x profiles=%d\n",
                status, count);
        context.vtable->vaTerminate(&context);
        dlclose(library);
        return 1;
    }
    {
        VAEntrypoint none_entrypoints[1] = {VAEntrypointVLD};
        int none_count = -1;
        status = context.vtable->vaQueryConfigEntrypoints(
            &context, VAProfileNone, none_entrypoints, &none_count);
        if (status != VA_STATUS_SUCCESS || none_count != 0) {
            fprintf(stderr,
                    "ProfileNone compatibility query failed: "
                    "status=0x%08x count=%d\n",
                    status, none_count);
            goto fail;
        }
    }
    for (i = 0; i < count; ++i) {
        VAEntrypoint entrypoints[4];
        int entrypoint_count = 0;
        int j;
        if (profiles[i] != VAProfileH264ConstrainedBaseline &&
            profiles[i] != VAProfileH264Main &&
            profiles[i] != VAProfileHEVCMain) {
            fprintf(stderr, "unexpected gated profile %d\n", profiles[i]);
            goto fail;
        }
        for (j = 0; j < i; ++j) {
            if (profiles[i] == profiles[j]) {
                fprintf(stderr, "duplicate gated profile\n");
                goto fail;
            }
        }
        status = context.vtable->vaQueryConfigEntrypoints(
            &context, profiles[i], entrypoints, &entrypoint_count);
        if (status != VA_STATUS_SUCCESS || entrypoint_count < 1 ||
            entrypoint_count > 2) {
            fprintf(stderr, "entrypoint query failed for profile %d\n",
                    profiles[i]);
            goto fail;
        }
        for (j = 0; j < entrypoint_count; ++j) {
            if (entrypoints[j] != VAEntrypointVLD &&
                entrypoints[j] != VAEntrypointEncSlice) {
                fprintf(stderr, "unexpected entrypoint %d\n",
                        entrypoints[j]);
                goto fail;
            }
            if (entrypoints[j] == VAEntrypointEncSlice) {
                VAConfigAttrib attrs[6];
                VAConfigID config = VA_INVALID_ID;
                VASurfaceAttrib surface_attrs[8];
                unsigned int surface_attr_count = 8;
                int found_prime2 = 0;
                int k;
                attrs[0].type = VAConfigAttribRTFormat;
                attrs[1].type = VAConfigAttribRateControl;
                attrs[2].type = VAConfigAttribEncPackedHeaders;
                attrs[3].type = VAConfigAttribEncInterlaced;
                attrs[4].type = VAConfigAttribEncMaxRefFrames;
                attrs[5].type = VAConfigAttribEncMaxSlices;
                status = context.vtable->vaGetConfigAttributes(
                    &context, profiles[i], entrypoints[j], attrs, 6);
                if (status != VA_STATUS_SUCCESS ||
                    attrs[0].value != VA_RT_FORMAT_YUV420 ||
                    attrs[1].value != VA_RC_VBR || attrs[2].value != 0 ||
                    attrs[3].value != 0 || attrs[4].value != 1 ||
                    attrs[5].value != 1) {
                    fprintf(stderr, "invalid EncSlice attributes\n");
                    goto fail;
                }
                status = context.vtable->vaCreateConfig(
                    &context, profiles[i], entrypoints[j], attrs, 2,
                    &config);
                if (status != VA_STATUS_SUCCESS || config == VA_INVALID_ID) {
                    fprintf(stderr, "EncSlice config creation failed\n");
                    goto fail;
                }
                status = context.vtable->vaQuerySurfaceAttributes(
                    &context, config, surface_attrs, &surface_attr_count);
                if (status != VA_STATUS_SUCCESS ||
                    surface_attr_count != 7) {
                    fprintf(stderr,
                            "EncSlice surface attribute query failed\n");
                    (void)context.vtable->vaDestroyConfig(&context, config);
                    goto fail;
                }
                for (k = 0; k < (int)surface_attr_count; ++k) {
                    if (surface_attrs[k].type == VASurfaceAttribMemoryType) {
                        if (surface_attrs[k].value.type !=
                                VAGenericValueTypeInteger ||
                            ((uint32_t)surface_attrs[k].value.value.i &
                             VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
                            fprintf(stderr, "invalid PRIME2 advertisement\n");
                            (void)context.vtable->vaDestroyConfig(&context,
                                                                  config);
                            goto fail;
                        }
                        found_prime2 = 1;
                    }
                }
                if (!found_prime2) {
                    fprintf(stderr, "PRIME2 not advertised\n");
                    (void)context.vtable->vaDestroyConfig(&context, config);
                    goto fail;
                }
                if (context.vtable->vaDestroyConfig(&context, config) !=
                    VA_STATUS_SUCCESS) {
                    fprintf(stderr, "EncSlice config destruction failed\n");
                    goto fail;
                }
            }
        }
    }
    status = context.vtable->vaQueryImageFormats(
        &context, image_formats, &image_count);
    if (status != VA_STATUS_SUCCESS || image_count != 2 ||
        image_formats[0].fourcc != VA_FOURCC_NV12 ||
        image_formats[1].fourcc != VA_FOURCC_I420) {
        fprintf(stderr, "unexpected image formats status=0x%08x count=%d\n",
                status, image_count);
        goto fail;
    }
    printf("vendor=%s profiles=%d image_formats=%d result=%s\n",
           context.str_vendor, count, image_count,
           count == 0 ? "fail-closed-pass" : "gated-codec-pass");
    context.vtable->vaTerminate(&context);
    dlclose(library);
    return 0;
fail:
    context.vtable->vaTerminate(&context);
    dlclose(library);
    return 1;
}
