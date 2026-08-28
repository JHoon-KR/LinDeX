#define _GNU_SOURCE

#include "advc_vaapi_image.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct fixture {
    struct advc_dmabuf_descriptor surface;
    unsigned int acquire_count;
    unsigned int release_count;
    unsigned int successful_release_count;
    unsigned int sync_count;
};

static void descriptor_init(struct advc_dmabuf_descriptor *descriptor) {
    uint32_t i;
    memset(descriptor, 0, sizeof(*descriptor));
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        descriptor->objects[i].fd = -1;
}

static int duplicate_descriptor(
    const struct advc_dmabuf_descriptor *source,
    struct advc_dmabuf_descriptor *destination) {
    uint32_t i;
    *destination = *source;
    for (i = 0; i < ADVC_MAX_DMABUF_OBJECTS; ++i)
        destination->objects[i].fd = -1;
    for (i = 0; i < source->object_count; ++i) {
        destination->objects[i].fd =
            fcntl(source->objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (destination->objects[i].fd < 0) {
            advc_dmabuf_descriptor_close(destination);
            return -1;
        }
    }
    return 0;
}

static int create_memfd(size_t size) {
    int fd = memfd_create("lindex-vaapi-image-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void make_linear_surface(struct advc_dmabuf_descriptor *descriptor,
                                uint64_t id, uint32_t width,
                                uint32_t height, uint32_t pitch) {
    uint64_t y_size = (uint64_t)pitch * height;
    uint64_t size = y_size + (uint64_t)pitch * (height / 2u);
    descriptor_init(descriptor);
    descriptor->buffer_id = id;
    descriptor->width = width;
    descriptor->height = height;
    descriptor->drm_fourcc = VA_FOURCC_NV12;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = 0;
    descriptor->crop_width = width;
    descriptor->crop_height = height;
    descriptor->object_count = 1;
    descriptor->plane_count = 2;
    descriptor->color_primaries = ADVC_COLOR_PRIMARIES_BT709;
    descriptor->color_transfer = ADVC_COLOR_TRANSFER_BT709;
    descriptor->color_matrix = ADVC_COLOR_MATRIX_BT709;
    descriptor->color_range = ADVC_COLOR_RANGE_LIMITED;
    descriptor->chroma_horizontal = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->chroma_vertical = ADVC_CHROMA_SITING_MIDPOINT;
    descriptor->objects[0].fd = create_memfd((size_t)size);
    assert(descriptor->objects[0].fd >= 0);
    descriptor->objects[0].size = size;
    descriptor->planes[0].pitch = pitch;
    descriptor->planes[1].offset = y_size;
    descriptor->planes[1].pitch = pitch;
    assert(advc_dmabuf_descriptor_validate(descriptor) == 0);
}

static VAStatus acquire_surface(
    void *opaque, VASurfaceID surface, enum advc_vaapi_surface_access access,
    struct advc_dmabuf_descriptor *descriptor, int *acquire_fence_fd) {
    struct fixture *fixture = opaque;
    assert(surface == 77);
    assert(access == ADVC_VAAPI_SURFACE_ACCESS_WRITE ||
           access == ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE);
    if (duplicate_descriptor(&fixture->surface, descriptor) < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    *acquire_fence_fd = -1;
    ++fixture->acquire_count;
    return VA_STATUS_SUCCESS;
}

static void release_surface(void *opaque, VASurfaceID surface,
                            enum advc_vaapi_surface_access access,
                            int access_succeeded) {
    struct fixture *fixture = opaque;
    assert(surface == 77);
    assert(access == ADVC_VAAPI_SURFACE_ACCESS_WRITE ||
           access == ADVC_VAAPI_SURFACE_ACCESS_READ_WRITE);
    ++fixture->release_count;
    if (access_succeeded) ++fixture->successful_release_count;
}

static int fake_dma_sync(void *opaque, int fd, uint64_t flags) {
    struct fixture *fixture = opaque;
    assert(fcntl(fd, F_GETFD) >= 0);
    assert(flags == 3 || flags == 7);
    ++fixture->sync_count;
    return 0;
}

static int fake_wait_fence(void *opaque, int fd, uint32_t timeout_ms) {
    (void)opaque;
    assert(fd >= 0);
    assert(timeout_ms == ADVC_VAAPI_IMAGE_SYNC_TIMEOUT_MS);
    return 0;
}

static struct advc_vaapi_image_runtime *make_runtime(struct fixture *fixture) {
    struct advc_vaapi_image_surface_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.opaque = fixture;
    ops.acquire_surface = acquire_surface;
    ops.release_surface = release_surface;
    ops.dma_buf_sync = fake_dma_sync;
    ops.wait_fence = fake_wait_fence;
    return advc_vaapi_image_runtime_create(&ops);
}

static void test_formats_and_nv12_upload(struct fixture *fixture) {
    struct advc_vaapi_image_runtime *runtime = make_runtime(fixture);
    VAImageFormat formats[ADVC_VAAPI_IMAGE_FORMAT_COUNT];
    VAImage image;
    uint8_t *data;
    uint8_t *surface;
    int count = 0;
    uint32_t row;
    assert(runtime != NULL);
    assert(advc_vaapi_image_query_formats(formats, &count) ==
           VA_STATUS_SUCCESS);
    assert(count == 2 && formats[0].fourcc == VA_FOURCC_NV12 &&
           formats[1].fourcc == VA_FOURCC_I420);
    assert(advc_vaapi_image_create(runtime, &formats[0], 64, 32, &image) ==
           VA_STATUS_SUCCESS);
    assert(image.num_planes == 2 && image.pitches[0] == 64 &&
           image.offsets[1] == 2048);
    assert(advc_vaapi_image_owns_buffer(runtime, image.buf));
    assert(advc_vaapi_image_map_buffer(runtime, image.buf,
                                       (void **)&data) == VA_STATUS_SUCCESS);
    for (row = 0; row < 32; ++row)
        memset(data + image.offsets[0] + row * image.pitches[0],
               (int)(row + 1u), 64);
    memset(data + image.offsets[1], 0x80, 64u * 16u);
    assert(advc_vaapi_image_unmap_buffer(runtime, image.buf) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_image_cpu_pixel_copy_count(runtime) == 0);
    assert(advc_vaapi_image_put(runtime, 77, image.image_id, 0, 0, 64, 32,
                                0, 0, 64, 32) == VA_STATUS_SUCCESS);
    assert(advc_vaapi_image_cpu_pixel_copy_count(runtime) == 1);
    surface = mmap(NULL, (size_t)fixture->surface.objects[0].size,
                   PROT_READ, MAP_SHARED, fixture->surface.objects[0].fd, 0);
    assert(surface != MAP_FAILED);
    for (row = 0; row < 32; ++row) {
        assert(surface[row * 80u] == (uint8_t)(row + 1u));
        assert(surface[row * 80u + 63u] == (uint8_t)(row + 1u));
        assert(surface[row * 80u + 64u] == 0);
    }
    assert(surface[fixture->surface.planes[1].offset] == 0x80);
    assert(surface[fixture->surface.planes[1].offset + 63u] == 0x80);
    munmap(surface, (size_t)fixture->surface.objects[0].size);
    assert(advc_vaapi_image_destroy(runtime, image.image_id) ==
           VA_STATUS_SUCCESS);
    assert(fixture->acquire_count == 1 && fixture->release_count == 1 &&
           fixture->successful_release_count == 1);
    advc_vaapi_image_runtime_destroy(runtime);
}

static void test_i420_upload(struct fixture *fixture) {
    struct advc_vaapi_image_runtime *runtime = make_runtime(fixture);
    VAImageFormat format;
    VAImage image;
    uint8_t *data;
    uint8_t *surface;
    size_t uv_offset = (size_t)fixture->surface.planes[1].offset;
    memset(&format, 0, sizeof(format));
    format.fourcc = VA_FOURCC_I420;
    assert(advc_vaapi_image_create(runtime, &format, 64, 32, &image) ==
           VA_STATUS_SUCCESS);
    assert(image.num_planes == 3);
    assert(advc_vaapi_image_map_buffer(runtime, image.buf,
                                       (void **)&data) == VA_STATUS_SUCCESS);
    memset(data + image.offsets[0], 0x10, 64u * 32u);
    memset(data + image.offsets[1], 0x22, 32u * 16u);
    memset(data + image.offsets[2], 0xdd, 32u * 16u);
    assert(advc_vaapi_image_unmap_buffer(runtime, image.buf) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_image_put(runtime, 77, image.image_id, 0, 0, 64, 32,
                                0, 0, 64, 32) == VA_STATUS_SUCCESS);
    assert(advc_vaapi_image_cpu_pixel_copy_count(runtime) == 1);
    surface = mmap(NULL, (size_t)fixture->surface.objects[0].size,
                   PROT_READ, MAP_SHARED, fixture->surface.objects[0].fd, 0);
    assert(surface != MAP_FAILED);
    assert(surface[uv_offset] == 0x22 && surface[uv_offset + 1u] == 0xdd &&
           surface[uv_offset + 62u] == 0x22 &&
           surface[uv_offset + 63u] == 0xdd);
    munmap(surface, (size_t)fixture->surface.objects[0].size);
    assert(advc_vaapi_image_put(runtime, 77, image.image_id, 1, 0, 62, 32,
                                0, 0, 62, 32) ==
           VA_STATUS_ERROR_INVALID_PARAMETER);
    assert(advc_vaapi_image_cpu_pixel_copy_count(runtime) == 1);
    assert(advc_vaapi_image_destroy(runtime, image.image_id) ==
           VA_STATUS_SUCCESS);
    advc_vaapi_image_runtime_destroy(runtime);
}

static void test_derive(struct fixture *fixture) {
    struct advc_vaapi_image_runtime *runtime = make_runtime(fixture);
    VAImage image;
    uint8_t *mapped;
    unsigned int releases = fixture->release_count;
    assert(advc_vaapi_image_derive(runtime, 77, &image) == VA_STATUS_SUCCESS);
    assert(image.format.fourcc == VA_FOURCC_NV12 &&
           image.pitches[0] == 80 && image.offsets[1] == 2560);
    assert(advc_vaapi_image_map_buffer(runtime, image.buf,
                                       (void **)&mapped) == VA_STATUS_SUCCESS);
    mapped[3] = 0x7a;
    assert(advc_vaapi_image_unmap_buffer(runtime, image.buf) ==
           VA_STATUS_SUCCESS);
    assert(advc_vaapi_image_destroy(runtime, image.image_id) ==
           VA_STATUS_SUCCESS);
    assert(fixture->release_count == releases + 1u);
    advc_vaapi_image_runtime_destroy(runtime);
}

static void fill_prime(const struct advc_dmabuf_descriptor *descriptor,
                       VADRMPRIMESurfaceDescriptor *prime) {
    memset(prime, 0, sizeof(*prime));
    prime->fourcc = VA_FOURCC_NV12;
    prime->width = descriptor->width;
    prime->height = descriptor->height;
    prime->num_objects = 1;
    prime->objects[0].fd = descriptor->objects[0].fd;
    prime->objects[0].size = (uint32_t)descriptor->objects[0].size;
    prime->objects[0].drm_format_modifier = 0;
    prime->num_layers = 1;
    prime->layers[0].drm_format = VA_FOURCC_NV12;
    prime->layers[0].num_planes = 2;
    prime->layers[0].object_index[0] = 0;
    prime->layers[0].object_index[1] = 0;
    prime->layers[0].offset[0] = 0;
    prime->layers[0].offset[1] =
        (uint32_t)descriptor->planes[1].offset;
    prime->layers[0].pitch[0] = descriptor->planes[0].pitch;
    prime->layers[0].pitch[1] = descriptor->planes[1].pitch;
}

static void test_prime_import_export(struct fixture *fixture) {
    VADRMPRIMESurfaceDescriptor prime;
    VADRMPRIMESurfaceDescriptor exported;
    struct advc_dmabuf_descriptor imported;
    VASurfaceAttrib attributes[3];
    int original_fd;
    int application_fd;
    int invalid_fence;
    fill_prime(&fixture->surface, &prime);
    original_fd = prime.objects[0].fd;
    application_fd = fcntl(original_fd, F_DUPFD_CLOEXEC, 0);
    assert(application_fd >= 0);
    prime.objects[0].fd = application_fd;
    assert(advc_vaapi_prime_import_nv12_linear(
               &prime, 9001, 64, 32, &imported) == VA_STATUS_SUCCESS);
    assert(imported.objects[0].fd != application_fd &&
           imported.drm_modifier == 0 && imported.planes[1].offset == 2560);
    close(application_fd);
    prime.objects[0].fd = original_fd;
    assert(fcntl(imported.objects[0].fd, F_GETFD) >= 0);
    assert(advc_vaapi_prime_export_nv12(
               &imported, -1,
               VA_EXPORT_SURFACE_READ_ONLY |
                   VA_EXPORT_SURFACE_COMPOSED_LAYERS,
               &exported) == VA_STATUS_SUCCESS);
    assert(exported.objects[0].fd >= 0 &&
           exported.objects[0].fd != imported.objects[0].fd &&
           exported.num_layers == 1 &&
           exported.layers[0].num_planes == 2);
    advc_vaapi_prime_export_close(&exported);
    assert(advc_vaapi_prime_export_nv12(
               &imported, -1,
               VA_EXPORT_SURFACE_READ_ONLY |
                   VA_EXPORT_SURFACE_SEPARATE_LAYERS,
               &exported) == VA_STATUS_SUCCESS);
    assert(exported.num_layers == 2 &&
           exported.layers[0].num_planes == 1 &&
           exported.layers[1].num_planes == 1);
    advc_vaapi_prime_export_close(&exported);
    assert(advc_vaapi_prime_export_nv12(
               &imported, -1,
               VA_EXPORT_SURFACE_WRITE_ONLY |
                   VA_EXPORT_SURFACE_SEPARATE_LAYERS,
               &exported) == VA_STATUS_SUCCESS);
    assert(exported.num_layers == 2 && exported.objects[0].fd >= 0);
    advc_vaapi_prime_export_close(&exported);

    memset(attributes, 0, sizeof(attributes));
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &prime;
    attributes[2].type = VASurfaceAttribPixelFormat;
    attributes[2].value.type = VAGenericValueTypeInteger;
    attributes[2].value.value.i = VA_FOURCC_NV12;
    {
        struct advc_dmabuf_descriptor from_attributes;
        assert(advc_vaapi_prime_import_surface_attributes(
                   VA_RT_FORMAT_YUV420, 64, 32, attributes, 3, 0, 1, 9002,
                   &from_attributes) == VA_STATUS_SUCCESS);
        advc_dmabuf_descriptor_close(&from_attributes);
    }

    prime.objects[0].drm_format_modifier = UINT64_C(1);
    {
        struct advc_dmabuf_descriptor rejected;
        assert(advc_vaapi_prime_import_nv12_linear(
                   &prime, 9003, 64, 32, &rejected) ==
               VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }
    prime.objects[0].drm_format_modifier =
        UINT64_C(0x0500000000000001);
    {
        struct advc_dmabuf_descriptor qcom;
        assert(advc_vaapi_prime_import_nv12_modifier(
                   &prime, 9005, 64, 32,
                   UINT64_C(0x0500000000000001), &qcom) ==
               VA_STATUS_SUCCESS);
        assert(qcom.drm_modifier == UINT64_C(0x0500000000000001) &&
               qcom.object_count == 1 && qcom.plane_count == 2);
        assert(advc_vaapi_prime_export_nv12(
                   &qcom, -1, VA_EXPORT_SURFACE_READ_ONLY, &exported) ==
               VA_STATUS_SUCCESS);
        assert(exported.objects[0].drm_format_modifier ==
               UINT64_C(0x0500000000000001));
        advc_vaapi_prime_export_close(&exported);
        assert(advc_vaapi_prime_export_nv12(
                   &qcom, -1, VA_EXPORT_SURFACE_WRITE_ONLY, &exported) ==
               VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        advc_dmabuf_descriptor_close(&qcom);
    }
    prime.objects[0].drm_format_modifier = 0;
    prime.layers[0].pitch[1] = 32;
    {
        struct advc_dmabuf_descriptor rejected;
        assert(advc_vaapi_prime_import_nv12_linear(
                   &prime, 9004, 64, 32, &rejected) ==
               VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }
    prime.layers[0].pitch[1] = 80;
    invalid_fence = fcntl(original_fd, F_DUPFD_CLOEXEC, 0);
    assert(invalid_fence >= 0);
    assert(advc_vaapi_prime_export_nv12(
               &imported, invalid_fence, VA_EXPORT_SURFACE_READ_ONLY,
               &exported) != VA_STATUS_SUCCESS);
    close(invalid_fence);
    advc_dmabuf_descriptor_close(&imported);
}

static void test_encode_link_validation(struct fixture *fixture) {
    struct advc_vaapi_encode_surface_link link;
    int invalid_fence;
    advc_vaapi_encode_surface_link_init(&link, &fixture->surface);
    assert(link.descriptor == &fixture->surface &&
           link.acquire_fence_fd == -1 && link.release_fence_fd == -1);
    invalid_fence = fcntl(fixture->surface.objects[0].fd, F_DUPFD_CLOEXEC, 0);
    assert(invalid_fence >= 0);
    assert(advc_vaapi_encode_surface_link_set_acquire_fence(
               &link, invalid_fence) < 0);
    close(invalid_fence);
    assert(advc_vaapi_encode_surface_link_take_release_fence(&link) == -1);
    advc_vaapi_encode_surface_link_close(&link);
}

int main(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    make_linear_surface(&fixture.surface, 100, 64, 32, 80);
    test_formats_and_nv12_upload(&fixture);
    test_i420_upload(&fixture);
    test_derive(&fixture);
    test_prime_import_export(&fixture);
    test_encode_link_validation(&fixture);
    assert(fixture.sync_count >= 6);
    advc_dmabuf_descriptor_close(&fixture.surface);
    return 0;
}
