#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int find_idr(const uint8_t *data, size_t size, const uint8_t **nal,
                    size_t *nal_size) {
    size_t i;
    for (i = 0; i + 4 < size; ++i) {
        size_t prefix = 0;
        size_t header;
        size_t end;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            prefix = 3;
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1)
            prefix = 4;
        if (prefix == 0) continue;
        header = i + prefix;
        if ((data[header] & 0x1fu) != 5u) continue;
        end = header + 1;
        while (end + 3 < size) {
            if (data[end] == 0 && data[end + 1] == 0 &&
                (data[end + 2] == 1 ||
                 (end + 3 < size && data[end + 2] == 0 &&
                  data[end + 3] == 1)))
                break;
            ++end;
        }
        if (end + 3 >= size) end = size;
        if (end <= header + 1) return -1;
        *nal = data + header;
        *nal_size = end - header;
        return 0;
    }
    return -1;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file;
    long length;
    uint8_t *data;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) !=
                            (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static int check_status(const char *stage, VAStatus status) {
    if (status == VA_STATUS_SUCCESS) return 0;
    fprintf(stderr, "%s: VA status 0x%08x\n", stage, status);
    return -1;
}

int main(int argc, char **argv) {
    struct VADriverContext context;
    struct VADriverVTable vtable;
    VADriverInit init;
    VAPictureParameterBufferH264 picture;
    VASliceParameterBufferH264 slice;
    VADRMPRIMESurfaceDescriptor prime;
    VAConfigAttrib attribute;
    VAConfigID config = VA_INVALID_ID;
    VAContextID decode = VA_INVALID_ID;
    VASurfaceID surfaces[4] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE,
                               VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VABufferID buffers[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    const uint8_t *idr;
    uint8_t *file_data = NULL;
    size_t file_size = 0;
    size_t idr_size = 0;
    unsigned int i;
    int rc = 1;
    void *library;
    if (argc != 3) {
        fprintf(stderr, "usage: %s advc_drv_video.so main-profile.264\n",
                argv[0]);
        return 2;
    }
    file_data = read_file(argv[2], &file_size);
    if (file_data == NULL ||
        find_idr(file_data, file_size, &idr, &idr_size) < 0) {
        fprintf(stderr, "input has no readable IDR\n");
        goto out;
    }
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        goto out;
    }
    init = (VADriverInit)dlsym(library, "__vaDriverInit_1_0");
    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;
    if (init == NULL || check_status("init", init(&context)) < 0) {
        dlclose(library);
        goto out;
    }
    attribute.type = VAConfigAttribRTFormat;
    attribute.value = VA_RT_FORMAT_YUV420;
    if (check_status("create-config", vtable.vaCreateConfig(
                         &context, VAProfileH264Main, VAEntrypointVLD,
                         &attribute, 1, &config)) < 0 ||
        check_status("create-surface", vtable.vaCreateSurfaces2(
                         &context, VA_RT_FORMAT_YUV420, 320, 240, surfaces, 4,
                         NULL, 0)) < 0 ||
        check_status("create-context", vtable.vaCreateContext(
                         &context, config, 320, 240, VA_PROGRESSIVE, surfaces,
                         4, &decode)) < 0)
        goto terminate;

    memset(&picture, 0, sizeof(picture));
    picture.picture_width_in_mbs_minus1 = 19;
    picture.picture_height_in_mbs_minus1 = 14;
    picture.num_ref_frames = 3;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.direct_8x8_inference_flag = 1;
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    picture.pic_init_qp_minus26 = -3;
    picture.chroma_qp_index_offset = -2;
    picture.second_chroma_qp_index_offset = -2;
    picture.pic_fields.bits.entropy_coding_mode_flag = 1;
    picture.pic_fields.bits.weighted_pred_flag = 1;
    picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    memset(&slice, 0, sizeof(slice));
    slice.slice_data_size = (uint32_t)idr_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.slice_type = 2;
    for (i = 0; i < 4; ++i) {
        unsigned int b;
        if (check_status("picture-buffer", vtable.vaCreateBuffer(
                             &context, decode, VAPictureParameterBufferType,
                             sizeof(picture), 1, &picture, &buffers[0])) < 0 ||
            check_status("slice-parameter", vtable.vaCreateBuffer(
                             &context, decode, VASliceParameterBufferType,
                             sizeof(slice), 1, &slice, &buffers[1])) < 0 ||
            check_status("slice-data", vtable.vaCreateBuffer(
                             &context, decode, VASliceDataBufferType,
                             (unsigned int)idr_size, 1, (void *)idr,
                             &buffers[2])) < 0 ||
            check_status("begin", vtable.vaBeginPicture(&context, decode,
                                                        surfaces[i])) < 0 ||
            check_status("render", vtable.vaRenderPicture(&context, decode,
                                                          buffers, 3)) < 0 ||
            check_status("end", vtable.vaEndPicture(&context, decode)) < 0)
            goto terminate;
        for (b = 0; b < 3; ++b) {
            if (check_status("destroy-buffer", vtable.vaDestroyBuffer(
                                                   &context, buffers[b])) < 0)
                goto terminate;
            buffers[b] = VA_INVALID_ID;
        }
    }
    {
        VASurfaceStatus query_status = VASurfaceRendering;
        for (i = 0; i < 5000 && query_status != VASurfaceReady; ++i) {
            if (check_status("query-status", vtable.vaQuerySurfaceStatus(
                                                  &context, surfaces[0],
                                                  &query_status)) < 0)
                goto terminate;
            if (query_status != VASurfaceReady) usleep(1000);
        }
        if (query_status != VASurfaceReady) {
            fprintf(stderr, "query-status timed out before vaSyncSurface\n");
            goto terminate;
        }
        printf("decode-query-status-pass polls=%u\n", i);
    }
    if (check_status("sync", vtable.vaSyncSurface2(
                                 &context, surfaces[0],
                                 UINT64_C(5000000000))) < 0)
        goto terminate;
    memset(&prime, 0, sizeof(prime));
    if (check_status("export", vtable.vaExportSurfaceHandle(
                                   &context, surfaces[0],
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY |
                                       VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &prime)) < 0)
        goto terminate;
    printf("decode-prime-pass fourcc=0x%08x modifier=0x%016llx "
           "objects=%u layers=%u\n",
           prime.fourcc, (unsigned long long)prime.objects[0].drm_format_modifier,
           prime.num_objects, prime.num_layers);
    for (i = 0; i < prime.num_objects; ++i) close(prime.objects[i].fd);
    rc = 0;
terminate:
    for (i = 0; i < 3; ++i)
        if (buffers[i] != VA_INVALID_ID)
            (void)vtable.vaDestroyBuffer(&context, buffers[i]);
    if (decode != VA_INVALID_ID)
        (void)vtable.vaDestroyContext(&context, decode);
    if (surfaces[0] != VA_INVALID_SURFACE)
        (void)vtable.vaDestroySurfaces(&context, surfaces, 4);
    if (config != VA_INVALID_ID)
        (void)vtable.vaDestroyConfig(&context, config);
    (void)vtable.vaTerminate(&context);
    dlclose(library);
out:
    free(file_data);
    return rc;
}
