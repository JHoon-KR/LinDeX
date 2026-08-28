#define _GNU_SOURCE
#include "advc/ahb_prime_mapper.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_FORMAT_ABGR8888 UINT32_C(0x34324241)

struct fake_mapper {
    int exports;
    int releases;
    int fail_export;
};

static int fake_export(void *userdata, void *hardware_buffer,
                       const struct advc_ahb_public_metadata *metadata,
                       struct advc_dmabuf_descriptor *descriptor) {
    struct fake_mapper *fake = (struct fake_mapper *)userdata;
    int fd;
    assert(hardware_buffer == (void *)(uintptr_t)0x1234);
    ++fake->exports;
    if (fake->fail_export) {
        errno = ENOTSUP;
        return -1;
    }
    fd = memfd_create("advc-prime-mapper-test", MFD_CLOEXEC);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)metadata->width * metadata->height * 4) == 0);
    descriptor->width = metadata->width;
    descriptor->height = metadata->height;
    descriptor->drm_fourcc = DRM_FORMAT_ABGR8888;
    descriptor->explicit_flags = ADVC_DMABUF_EXPLICIT_ALL;
    descriptor->drm_modifier = 0;
    descriptor->crop_width = metadata->width;
    descriptor->crop_height = metadata->height;
    descriptor->object_count = 1;
    descriptor->plane_count = 1;
    descriptor->color_matrix = ADVC_COLOR_MATRIX_RGB;
    descriptor->color_range = ADVC_COLOR_RANGE_FULL;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size =
        (uint64_t)metadata->width * metadata->height * 4;
    descriptor->planes[0].pitch = metadata->width * 4;
    return 0;
}

static int fake_release(void *userdata, void *hardware_buffer,
                        int release_fence_fd) {
    struct fake_mapper *fake = (struct fake_mapper *)userdata;
    assert(hardware_buffer == (void *)(uintptr_t)0x1234);
    assert(release_fence_fd == -1);
    ++fake->releases;
    return 0;
}

int main(void) {
    static const struct advc_ahb_prime_mapper_ops ops = {
        .export_prime = fake_export,
        .release = fake_release,
    };
    struct advc_ahb_public_metadata metadata = {
        .width = 128,
        .height = 64,
        .android_format = 1,
        .stride = 128,
        .layers = 1,
        .usage = UINT64_C(0x100),
        .crop_width = 128,
        .crop_height = 64,
    };
    struct fake_mapper fake = {0};
    struct advc_ahb_prime_mapper *mapper;
    struct advc_ahb_prime_export exported;
    int arbitrary_fd;

    assert(advc_ahb_prime_mapper_create(NULL, NULL) == NULL && errno == EINVAL);
    mapper = advc_ahb_prime_mapper_create(&ops, &fake);
    assert(mapper != NULL);
    assert(advc_ahb_prime_mapper_export(
               mapper, (void *)(uintptr_t)0x1234, &metadata, 9, -1,
               &exported) == 0);
    assert(fake.exports == 1);
    assert(exported.descriptor.buffer_id == 9);
    assert(exported.descriptor.drm_fourcc == DRM_FORMAT_ABGR8888);
    assert(exported.descriptor.objects[0].fd >= 0);
    assert(exported.descriptor.planes[0].object_index == 0);
    assert(exported.descriptor.planes[0].offset == 0);
    assert(exported.descriptor.planes[0].pitch == 512);
    assert(exported.acquire_fence_fd == -1);
    advc_ahb_prime_export_close(&exported);
    assert(exported.descriptor.objects[0].fd == -1);

    arbitrary_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(arbitrary_fd >= 0);
    assert(advc_ahb_prime_mapper_export(
               mapper, (void *)(uintptr_t)0x1234, &metadata, 10,
               arbitrary_fd, &exported) < 0);
    close(arbitrary_fd);
    assert(fake.exports == 1); /* Rejected before the authoritative mapper. */

    fake.fail_export = 1;
    assert(advc_ahb_prime_mapper_export(
               mapper, (void *)(uintptr_t)0x1234, &metadata, 11, -1,
               &exported) < 0);
    assert(fake.exports == 2);
    assert(advc_ahb_prime_mapper_release(
               mapper, (void *)(uintptr_t)0x1234, -1) == 0);
    assert(fake.releases == 1);
    advc_ahb_prime_mapper_destroy(mapper);
    puts("ahb_prime_mapper_test: PASS");
    return 0;
}
