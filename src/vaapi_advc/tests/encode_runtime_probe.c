#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_hevc.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROBE_WIDTH 320u
#define PROBE_HEIGHT 240u
#define PROBE_CODED_CAPACITY (2u * 1024u * 1024u)

enum probe_codec {
    PROBE_CODEC_H264 = 0,
    PROBE_CODEC_HEVC = 1,
};

union probe_sequence {
    VAEncSequenceParameterBufferH264 h264;
    VAEncSequenceParameterBufferHEVC hevc;
};

union probe_picture {
    VAEncPictureParameterBufferH264 h264;
    VAEncPictureParameterBufferHEVC hevc;
};

union probe_slice {
    VAEncSliceParameterBufferH264 h264;
    VAEncSliceParameterBufferHEVC hevc;
};

static int check_status(const char *stage, VAStatus status) {
    if (status == VA_STATUS_SUCCESS) return 0;
    fprintf(stderr, "%s: VA status 0x%08x\n", stage, status);
    return -1;
}

static void fill_nv12(VAImage *image, uint8_t *data) {
    unsigned int x;
    unsigned int y;
    for (y = 0; y < PROBE_HEIGHT; ++y) {
        uint8_t *row = data + image->offsets[0] + y * image->pitches[0];
        for (x = 0; x < PROBE_WIDTH; ++x)
            row[x] = (uint8_t)(16u + ((x + y) % 220u));
    }
    for (y = 0; y < PROBE_HEIGHT / 2u; ++y) {
        uint8_t *row = data + image->offsets[1] + y * image->pitches[1];
        for (x = 0; x < PROBE_WIDTH; x += 2u) {
            row[x] = (uint8_t)(96u + (y % 64u));
            row[x + 1u] = (uint8_t)(160u - (y % 64u));
        }
    }
}

static void close_prime_descriptor(VADRMPRIMESurfaceDescriptor *descriptor) {
    uint32_t i;
    if (descriptor == NULL) return;
    for (i = 0; i < descriptor->num_objects; ++i) {
        if (descriptor->objects[i].fd >= 0) {
            close(descriptor->objects[i].fd);
            descriptor->objects[i].fd = -1;
        }
    }
}

int main(int argc, char **argv) {
    struct VADriverContext driver_context;
    struct VADriverVTable vtable;
    VADriverInit init;
    VAConfigAttrib config_attributes[2];
    VASurfaceAttrib surface_attributes[2];
    unsigned int queried_surface_attributes = 0;
    VAImageFormat image_format;
    VAImage image;
    union probe_sequence sequence;
    union probe_picture picture;
    union probe_slice slice;
    VACodedBufferSegment *segment;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VABufferID coded = VA_INVALID_ID;
    VABufferID parameters[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    FILE *output = NULL;
    const char *output_path = NULL;
    const char *codec_name = "h264";
    enum probe_codec codec = PROBE_CODEC_H264;
    VAProfile profile = VAProfileH264ConstrainedBaseline;
    void *sequence_data;
    void *picture_data;
    void *slice_data;
    unsigned int sequence_size;
    unsigned int picture_size;
    unsigned int slice_size;
    void *mapped = NULL;
    size_t total = 0;
    unsigned int i;
    int image_mapped = 0;
    int coded_mapped = 0;
    int repeated_export_first_open = 0;
    int repeated_export_second_open = 0;
    int rc = 1;
    void *library;

    if (argc < 2 || argc > 4) {
        fprintf(stderr,
                "usage: %s advc_drv_video.so [h264|hevc] [output]\n",
                argv[0]);
        return 2;
    }
    if (argc >= 3 &&
        (strcmp(argv[2], "h264") == 0 || strcmp(argv[2], "hevc") == 0)) {
        if (strcmp(argv[2], "hevc") == 0) {
            codec = PROBE_CODEC_HEVC;
            codec_name = "hevc";
            profile = VAProfileHEVCMain;
        }
        if (argc == 4) output_path = argv[3];
    } else if (argc == 3) {
        /* Preserve the original DRIVER OUTPUT.264 invocation. */
        output_path = argv[2];
    } else if (argc == 4) {
        fprintf(stderr, "codec must be h264 or hevc\n");
        return 2;
    }
    if (output_path != NULL) {
        output = fopen(output_path, "wb");
        if (output == NULL) {
            perror("output");
            return 1;
        }
    }
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        goto out;
    }
    init = (VADriverInit)dlsym(library, "__vaDriverInit_1_0");
    memset(&driver_context, 0, sizeof(driver_context));
    memset(&vtable, 0, sizeof(vtable));
    driver_context.vtable = &vtable;
    if (init == NULL ||
        check_status("init", init(&driver_context)) < 0)
        goto close_library;

    config_attributes[0].type = VAConfigAttribRTFormat;
    config_attributes[0].value = VA_RT_FORMAT_YUV420;
    config_attributes[1].type = VAConfigAttribRateControl;
    config_attributes[1].value = VA_RC_VBR;
    if (check_status("create-config", vtable.vaCreateConfig(
                         &driver_context, profile,
                         VAEntrypointEncSlice, config_attributes, 2,
                         &config)) < 0)
        goto terminate;
    if (check_status("query-surface-attributes",
                     vtable.vaQuerySurfaceAttributes(
                         &driver_context, config, NULL,
                         &queried_surface_attributes)) < 0 ||
        queried_surface_attributes == 0)
        goto terminate;

    memset(surface_attributes, 0, sizeof(surface_attributes));
    surface_attributes[0].type = VASurfaceAttribPixelFormat;
    surface_attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    surface_attributes[0].value.type = VAGenericValueTypeInteger;
    surface_attributes[0].value.value.i = VA_FOURCC_NV12;
    surface_attributes[1].type = VASurfaceAttribMemoryType;
    surface_attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    surface_attributes[1].value.type = VAGenericValueTypeInteger;
    surface_attributes[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    if (check_status("create-surface", vtable.vaCreateSurfaces2(
                         &driver_context, VA_RT_FORMAT_YUV420, PROBE_WIDTH,
                         PROBE_HEIGHT, &surface, 1, surface_attributes, 2)) < 0)
        goto terminate;

    memset(&image_format, 0, sizeof(image_format));
    image_format.fourcc = VA_FOURCC_NV12;
    image_format.byte_order = VA_LSB_FIRST;
    image_format.bits_per_pixel = 12;
    memset(&image, 0, sizeof(image));
    image.image_id = VA_INVALID_ID;
    if (check_status("create-image", vtable.vaCreateImage(
                         &driver_context, &image_format, PROBE_WIDTH,
                         PROBE_HEIGHT, &image)) < 0 ||
        check_status("map-image", vtable.vaMapBuffer(
                         &driver_context, image.buf, &mapped)) < 0)
        goto terminate;
    image_mapped = 1;
    fill_nv12(&image, mapped);
    if (check_status("unmap-image", vtable.vaUnmapBuffer(
                         &driver_context, image.buf)) < 0)
        goto terminate;
    image_mapped = 0;
    mapped = NULL;
    if (check_status("put-image", vtable.vaPutImage(
                         &driver_context, surface, image.image_id, 0, 0,
                         PROBE_WIDTH, PROBE_HEIGHT, 0, 0, PROBE_WIDTH,
                         PROBE_HEIGHT)) < 0 ||
        check_status("destroy-image", vtable.vaDestroyImage(
                         &driver_context, image.image_id)) < 0)
        goto terminate;
    image.image_id = VA_INVALID_ID;

    if (getenv("ADVC_PROBE_REPEATED_WRITE_EXPORT") != NULL &&
        strcmp(getenv("ADVC_PROBE_REPEATED_WRITE_EXPORT"),
               "validated-v1") == 0) {
        VADRMPRIMESurfaceDescriptor first;
        VADRMPRIMESurfaceDescriptor second;
        memset(&first, 0, sizeof(first));
        memset(&second, 0, sizeof(second));
        if (vtable.vaExportSurfaceHandle == NULL ||
            check_status("export-write-first", vtable.vaExportSurfaceHandle(
                             &driver_context, surface,
                             VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                             VA_EXPORT_SURFACE_WRITE_ONLY |
                                 VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                             &first)) < 0) {
            close_prime_descriptor(&first);
            goto terminate;
        }
        repeated_export_first_open = first.num_objects > 0 &&
                                     first.objects[0].fd >= 0;
        if (check_status("export-write-second", vtable.vaExportSurfaceHandle(
                             &driver_context, surface,
                             VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                             VA_EXPORT_SURFACE_WRITE_ONLY |
                                 VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                             &second)) < 0) {
            close_prime_descriptor(&first);
            close_prime_descriptor(&second);
            goto terminate;
        }
        repeated_export_second_open = second.num_objects > 0 &&
                                      second.objects[0].fd >= 0;
        if (!repeated_export_first_open || !repeated_export_second_open ||
            first.objects[0].fd == second.objects[0].fd) {
            fprintf(stderr, "repeated writable export did not return independent fds\n");
            close_prime_descriptor(&first);
            close_prime_descriptor(&second);
            goto terminate;
        }
        close_prime_descriptor(&first);
        if (fcntl(second.objects[0].fd, F_GETFD) < 0) {
            perror("second export fd after first close");
            close_prime_descriptor(&second);
            goto terminate;
        }
        close_prime_descriptor(&second);
        puts("repeated-write-export-pass independent-fds=1 fence=end-picture");
    }

    if (check_status("create-context", vtable.vaCreateContext(
                         &driver_context, config, PROBE_WIDTH, PROBE_HEIGHT,
                         VA_PROGRESSIVE, &surface, 1, &context)) < 0 ||
        check_status("create-coded", vtable.vaCreateBuffer(
                         &driver_context, context, VAEncCodedBufferType,
                         PROBE_CODED_CAPACITY, 1, NULL, &coded)) < 0)
        goto terminate;

    memset(&sequence, 0, sizeof(sequence));
    memset(&picture, 0, sizeof(picture));
    memset(&slice, 0, sizeof(slice));
    if (codec == PROBE_CODEC_H264) {
        sequence.h264.picture_width_in_mbs = PROBE_WIDTH / 16u;
        sequence.h264.picture_height_in_mbs = PROBE_HEIGHT / 16u;
        sequence.h264.seq_fields.bits.chroma_format_idc = 1;
        sequence.h264.seq_fields.bits.frame_mbs_only_flag = 1;
        sequence.h264.bits_per_second = 2000000;
        sequence.h264.intra_period = 60;
        sequence.h264.intra_idr_period = 60;
        picture.h264.coded_buf = coded;
        picture.h264.pic_fields.bits.idr_pic_flag = 1;
        slice.h264.num_macroblocks =
            (PROBE_WIDTH / 16u) * (PROBE_HEIGHT / 16u);
        slice.h264.slice_type = 2;
        sequence_data = &sequence.h264;
        sequence_size = sizeof(sequence.h264);
        picture_data = &picture.h264;
        picture_size = sizeof(picture.h264);
        slice_data = &slice.h264;
        slice_size = sizeof(slice.h264);
    } else {
        sequence.hevc.general_profile_idc = 1;
        sequence.hevc.pic_width_in_luma_samples = PROBE_WIDTH;
        sequence.hevc.pic_height_in_luma_samples = PROBE_HEIGHT;
        sequence.hevc.seq_fields.bits.chroma_format_idc = 1;
        sequence.hevc.bits_per_second = 2000000;
        sequence.hevc.intra_period = 60;
        sequence.hevc.intra_idr_period = 60;
        picture.hevc.coded_buf = coded;
        picture.hevc.pic_fields.bits.idr_pic_flag = 1;
        picture.hevc.pic_fields.bits.coding_type = 1;
        slice.hevc.num_ctu_in_slice =
            (PROBE_WIDTH / 16u) * (PROBE_HEIGHT / 16u);
        slice.hevc.slice_type = 2;
        slice.hevc.slice_fields.bits.last_slice_of_pic_flag = 1;
        sequence_data = &sequence.hevc;
        sequence_size = sizeof(sequence.hevc);
        picture_data = &picture.hevc;
        picture_size = sizeof(picture.hevc);
        slice_data = &slice.hevc;
        slice_size = sizeof(slice.hevc);
    }
    if (check_status("sequence-buffer", vtable.vaCreateBuffer(
                         &driver_context, context,
                         VAEncSequenceParameterBufferType, sequence_size,
                         1, sequence_data, &parameters[0])) < 0 ||
        check_status("picture-buffer", vtable.vaCreateBuffer(
                         &driver_context, context,
                         VAEncPictureParameterBufferType, picture_size, 1,
                         picture_data, &parameters[1])) < 0 ||
        check_status("slice-buffer", vtable.vaCreateBuffer(
                         &driver_context, context,
                         VAEncSliceParameterBufferType, slice_size, 1,
                         slice_data, &parameters[2])) < 0 ||
        check_status("begin", vtable.vaBeginPicture(
                         &driver_context, context, surface)) < 0 ||
        check_status("render", vtable.vaRenderPicture(
                         &driver_context, context, parameters, 3)) < 0 ||
        check_status("end", vtable.vaEndPicture(
                         &driver_context, context)) < 0 ||
        vtable.vaSyncBuffer == NULL ||
        check_status("sync-buffer", vtable.vaSyncBuffer(
                         &driver_context, coded,
                         UINT64_C(5000000000))) < 0 ||
        check_status("sync-surface", vtable.vaSyncSurface2(
                         &driver_context, surface,
                         UINT64_C(5000000000))) < 0 ||
        check_status("map-coded", vtable.vaMapBuffer(
                         &driver_context, coded, &mapped)) < 0)
        goto terminate;
    coded_mapped = 1;
    for (segment = mapped; segment != NULL; segment = segment->next) {
        if (segment->size > 0 && segment->buf == NULL) {
            fprintf(stderr, "coded segment has no data\n");
            goto terminate;
        }
        if (output != NULL && segment->size > 0 &&
            fwrite(segment->buf, 1, segment->size, output) != segment->size) {
            perror("write output");
            goto terminate;
        }
        total += segment->size;
    }
    if (total == 0) {
        fprintf(stderr, "encoder returned an empty coded buffer\n");
        goto terminate;
    }
    if (check_status("unmap-coded", vtable.vaUnmapBuffer(
                         &driver_context, coded)) < 0)
        goto terminate;
    coded_mapped = 0;
    mapped = NULL;
    printf("encode-vaapi-pass codec=%s profile=%s entrypoint=EncSlice "
           "bytes=%zu\n", codec_name,
           codec == PROBE_CODEC_H264 ? "constrained-baseline" : "main",
           total);
    rc = 0;

terminate:
    if (coded_mapped)
        (void)vtable.vaUnmapBuffer(&driver_context, coded);
    if (image_mapped)
        (void)vtable.vaUnmapBuffer(&driver_context, image.buf);
    if (image.image_id != VA_INVALID_ID)
        (void)vtable.vaDestroyImage(&driver_context, image.image_id);
    for (i = 0; i < 3; ++i)
        if (parameters[i] != VA_INVALID_ID)
            (void)vtable.vaDestroyBuffer(&driver_context, parameters[i]);
    if (coded != VA_INVALID_ID)
        (void)vtable.vaDestroyBuffer(&driver_context, coded);
    if (context != VA_INVALID_ID)
        (void)vtable.vaDestroyContext(&driver_context, context);
    if (surface != VA_INVALID_SURFACE)
        (void)vtable.vaDestroySurfaces(&driver_context, &surface, 1);
    if (config != VA_INVALID_ID)
        (void)vtable.vaDestroyConfig(&driver_context, config);
    (void)vtable.vaTerminate(&driver_context);
close_library:
    dlclose(library);
out:
    if (output != NULL) fclose(output);
    return rc;
}
