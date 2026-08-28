#define _GNU_SOURCE

#include "advc_vaapi_encode_runtime.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int make_surface_fd(size_t size) {
    int fd = memfd_create("advc-vaapi-encode-runtime", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    return fd;
}

static void fill_prime(VADRMPRIMESurfaceDescriptor *prime, uint32_t width,
                       uint32_t height, int fd) {
    uint32_t y_size = width * height;
    memset(prime, 0, sizeof(*prime));
    prime->fourcc = VA_FOURCC_NV12;
    prime->width = width;
    prime->height = height;
    prime->num_objects = 1;
    prime->objects[0].fd = fd;
    prime->objects[0].size = y_size + y_size / 2u;
    prime->objects[0].drm_format_modifier = 0;
    prime->num_layers = 1;
    prime->layers[0].drm_format = VA_FOURCC_NV12;
    prime->layers[0].num_planes = 2;
    prime->layers[0].object_index[0] = 0;
    prime->layers[0].object_index[1] = 0;
    prime->layers[0].offset[0] = 0;
    prime->layers[0].offset[1] = y_size;
    prime->layers[0].pitch[0] = width;
    prime->layers[0].pitch[1] = width;
}

int main(void) {
    struct advc_vaapi_encode_policy policy;
    struct advc_vaapi_encode_runtime *runtime;
    VADRMPRIMESurfaceDescriptor prime;
    VADRMPRIMESurfaceDescriptor exported;
    VASurfaceAttrib surface_attrs[4];
    VASurfaceAttrib queried[8];
    VAConfigAttrib config_attrs[2];
    VAConfigAttrib queried_config[2];
    VAEncSequenceParameterBufferH264 sequence;
    VAEncPictureParameterBufferH264 picture;
    VAEncSliceParameterBufferH264 slice;
    VAConfigID config;
    VAConfigID rejected_config = VA_INVALID_ID;
    VAContextID context;
    VASurfaceID surface;
    VABufferID coded;
    VABufferID params[3];
    VAImage image;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAStatus status;
    struct advc_vaapi_encode_surface_accounting accounting;
    void *mapped = NULL;
    unsigned int attribute_count;
    int config_attribute_count;
    int fd;

    memset(&policy, 0, sizeof(policy));
    policy.codecs = ADVC_VAAPI_ENCODE_H264 | ADVC_VAAPI_ENCODE_HEVC;
    policy.h264_max_width = 4096;
    policy.h264_max_height = 2160;
    policy.hevc_max_width = 4096;
    policy.hevc_max_height = 2160;
    policy.rate_control = VA_RC_VBR;
    policy.rt_formats = VA_RT_FORMAT_YUV420;
    policy.prime_input_ready = 1;
    runtime = advc_vaapi_encode_runtime_create(
        "/tmp/advc-vaapi-encode-runtime-no-broker.sock", &policy);
    assert(runtime != NULL);

    config_attrs[0].type = VAConfigAttribRTFormat;
    config_attrs[0].value = VA_RT_FORMAT_YUV420;
    config_attrs[1].type = VAConfigAttribRateControl;
    config_attrs[1].value = VA_RC_VBR;
    assert(advc_vaapi_encode_create_config(
               runtime, VAProfileH264ConstrainedBaseline,
               VAEntrypointEncSlice, config_attrs, 2, &config) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_owns_config(config));
    assert(advc_vaapi_encode_create_config(
               runtime, VAProfileH264High, VAEntrypointEncSlice,
               config_attrs, 2, &rejected_config) ==
           VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    config_attribute_count = 0;
    assert(advc_vaapi_encode_query_config(
               runtime, config, &profile, &entrypoint, queried_config,
               &config_attribute_count) == VA_STATUS_SUCCESS);
    assert(profile == VAProfileH264ConstrainedBaseline &&
           entrypoint == VAEntrypointEncSlice &&
           config_attribute_count == 2);

    attribute_count = 0;
    assert(advc_vaapi_encode_query_surface_attributes(
               runtime, config, NULL, &attribute_count) ==
           VA_STATUS_SUCCESS);
    assert(attribute_count == 7);
    attribute_count = 8;
    assert(advc_vaapi_encode_query_surface_attributes(
               runtime, config, queried, &attribute_count) ==
           VA_STATUS_SUCCESS);
    assert(attribute_count == 7 &&
           queried[5].value.value.i ==
               (VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2));

    fd = make_surface_fd(64u * 32u * 3u / 2u);
    fill_prime(&prime, 64, 32, fd);
    memset(surface_attrs, 0, sizeof(surface_attrs));
    surface_attrs[0].type = VASurfaceAttribMemoryType;
    surface_attrs[0].value.type = VAGenericValueTypeInteger;
    surface_attrs[0].value.value.i =
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    surface_attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    surface_attrs[1].value.type = VAGenericValueTypePointer;
    surface_attrs[1].value.value.p = &prime;
    surface_attrs[2].type = VASurfaceAttribPixelFormat;
    surface_attrs[2].value.type = VAGenericValueTypeInteger;
    surface_attrs[2].value.value.i = VA_FOURCC_NV12;
    surface_attrs[3].type = VASurfaceAttribUsageHint;
    surface_attrs[3].value.type = VAGenericValueTypeInteger;
    surface_attrs[3].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;
    assert(advc_vaapi_encode_create_surfaces(
               runtime, VA_RT_FORMAT_YUV420, 64, 32, &surface, 1,
               surface_attrs, 4) == VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_owns_surface(surface));
    memset(&accounting, 0xff, sizeof(accounting));
    assert(advc_vaapi_encode_query_surface_accounting(
               runtime, surface, &accounting) == VA_STATUS_SUCCESS);
    assert(accounting.cpu_pixel_copies == 0 &&
           accounting.writable_prime_exports == 0 &&
           accounting.gpu_conversion_submits == 0 &&
           accounting.gpu_modifier_repack_submits == 0);
    close(fd);

    assert(advc_vaapi_encode_export_surface(
               runtime, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
               VA_EXPORT_SURFACE_READ_ONLY |
                   VA_EXPORT_SURFACE_COMPOSED_LAYERS,
               &exported) == VA_STATUS_SUCCESS);
    assert(exported.fourcc == VA_FOURCC_NV12 && exported.num_layers == 1);
    advc_vaapi_prime_export_close(&exported);
    assert(advc_vaapi_encode_export_surface(
               runtime, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
               VA_EXPORT_SURFACE_WRITE_ONLY,
               &exported) != VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_query_surface_accounting(
               runtime, surface, &accounting) == VA_STATUS_SUCCESS);
    assert(accounting.cpu_pixel_copies == 0 &&
           accounting.writable_prime_exports == 0 &&
           accounting.gpu_conversion_submits == 0 &&
           accounting.gpu_modifier_repack_submits == 0);

    memset(&image, 0, sizeof(image));
    assert(advc_vaapi_image_derive(
               advc_vaapi_encode_image_runtime(runtime), surface, &image) ==
           VA_STATUS_SUCCESS);
    assert(image.format.fourcc == VA_FOURCC_NV12);
    assert(advc_vaapi_image_destroy(
               advc_vaapi_encode_image_runtime(runtime), image.image_id) ==
           VA_STATUS_SUCCESS);

    assert(advc_vaapi_encode_create_context(runtime, config, 64, 32, 0,
                                            &surface, 1, &context) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_owns_context(context));
    assert(advc_vaapi_encode_create_buffer(
               runtime, context, VAEncCodedBufferType, 65536, 1, NULL,
               &coded) == VA_STATUS_SUCCESS);
    memset(&sequence, 0, sizeof(sequence));
    sequence.picture_width_in_mbs = 4;
    sequence.picture_height_in_mbs = 2;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    sequence.bits_per_second = 2000000;
    memset(&picture, 0, sizeof(picture));
    picture.coded_buf = coded;
    picture.pic_fields.bits.idr_pic_flag = 1;
    memset(&slice, 0, sizeof(slice));
    slice.num_macroblocks = 8;
    slice.slice_type = 2;
    assert(advc_vaapi_encode_create_buffer(
               runtime, context, VAEncSequenceParameterBufferType,
               sizeof(sequence), 1, &sequence, &params[0]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_create_buffer(
               runtime, context, VAEncPictureParameterBufferType,
               sizeof(picture), 1, &picture, &params[1]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_create_buffer(
               runtime, context, VAEncSliceParameterBufferType,
               sizeof(slice), 1, &slice, &params[2]) == VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_sync_buffer(runtime, coded, 0) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_sync_buffer(runtime, params[0], 0) ==
           VA_STATUS_ERROR_INVALID_BUFFER);
    assert(advc_vaapi_encode_sync_buffer(runtime, VA_INVALID_ID, 0) ==
           VA_STATUS_ERROR_INVALID_BUFFER);
    assert(advc_vaapi_encode_map_buffer(runtime, params[0], &mapped) ==
           VA_STATUS_SUCCESS);
    assert(mapped != NULL);
    assert(advc_vaapi_encode_unmap_buffer(runtime, params[0]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_begin_picture(runtime, context, surface) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_render_picture(runtime, context, params, 3) ==
           VA_STATUS_SUCCESS);
    status = advc_vaapi_encode_end_picture(runtime, context);
    assert(status != VA_STATUS_SUCCESS);

    assert(advc_vaapi_encode_destroy_buffer(runtime, params[0]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_buffer(runtime, params[1]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_buffer(runtime, params[2]) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_buffer(runtime, coded) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_context(runtime, context) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_surfaces(runtime, &surface, 1) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_encode_destroy_config(runtime, config) ==
           VA_STATUS_SUCCESS);
    advc_vaapi_encode_runtime_destroy(runtime);
    return 0;
}
