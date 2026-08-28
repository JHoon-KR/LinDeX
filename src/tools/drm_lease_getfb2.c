// SPDX-License-Identifier: MIT
// Read-only active framebuffer proof on an inherited DRM lease fd.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_FORMAT_XBGR8888 0x34324258U /* XB24 */
#define DRM_FORMAT_MOD_QCOM_COMPRESSED 0x0500000000000001ULL

struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs;
    uint32_t gamma_size, count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

_Static_assert(sizeof(struct drm_mode_get_plane) == 32, "getplane ABI");
_Static_assert(sizeof(struct drm_mode_fb_cmd2) == 104, "getfb2 ABI");
_Static_assert(sizeof(struct drm_gem_close) == 8, "gem close ABI");

#define DRM_IOCTL_MODE_GETPLANE \
    _IOWR(DRM_IOCTL_BASE, 0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_GETFB2 \
    _IOWR(DRM_IOCTL_BASE, 0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_GEM_CLOSE \
    _IOW(DRM_IOCTL_BASE, 0x09, struct drm_gem_close)

static int parse_positive_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (!text || !text[0] || text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_fd(const char *text)
{
    char *end = NULL;
    long parsed;

    if (!text || !text[0])
        return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < 3 || parsed > 1024)
        return -1;
    return (int)parsed;
}

static void close_returned_handles(int fd, const struct drm_mode_fb_cmd2 *fb)
{
    for (size_t i = 0; i < 4; ++i) {
        if (!fb->handles[i])
            continue;
        int duplicate = 0;
        for (size_t before = 0; before < i; ++before) {
            if (fb->handles[before] == fb->handles[i])
                duplicate = 1;
        }
        if (!duplicate) {
            struct drm_gem_close request = {.handle = fb->handles[i]};
            (void)ioctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
        }
    }
}

int main(int argc, char **argv)
{
    struct drm_mode_get_plane plane;
    struct drm_mode_fb_cmd2 fb;
    uint32_t expected_plane;
    int classify = 0;
    int inherited_fd, fd;

    if (argc == 3 && strcmp(argv[1], "--classify") == 0) {
        classify = 1;
        argv++;
        argc--;
    }
    if (argc != 2 || parse_positive_u32(argv[1], &expected_plane) != 0) {
        fprintf(stderr, "usage: drm_lease_getfb2 [--classify] PLANE_ID\n");
        return 64;
    }
    inherited_fd = parse_fd(getenv("DRM_LEASE_FD"));
    if (inherited_fd < 0 || fcntl(inherited_fd, F_GETFD) < 0) {
        fprintf(stderr, "LEASE_GETFB2 REFUSE: invalid DRM_LEASE_FD\n");
        if (classify)
            puts("LEASE_GETFB2 result=INDETERMINATE reason=invalid-lease-fd");
        return 78;
    }
    fd = fcntl(inherited_fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) {
        fprintf(stderr, "LEASE_GETFB2 REFUSE: fd duplication failed errno=%d (%s)\n",
                errno, strerror(errno));
        if (classify)
            puts("LEASE_GETFB2 result=INDETERMINATE reason=fd-duplication-failed");
        return 78;
    }

    memset(&plane, 0, sizeof(plane));
    plane.plane_id = expected_plane;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 ||
        plane.plane_id != expected_plane || !plane.crtc_id || !plane.fb_id) {
        fprintf(stderr,
                "LEASE_GETFB2 REFUSE: active plane unavailable plane=%u crtc=%u fb=%u errno=%d (%s)\n",
                expected_plane, plane.crtc_id, plane.fb_id, errno, strerror(errno));
        if (classify)
            puts("LEASE_GETFB2 result=INDETERMINATE reason=active-plane-unavailable");
        close(fd);
        return 78;
    }

    memset(&fb, 0, sizeof(fb));
    fb.fb_id = plane.fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb) != 0) {
        fprintf(stderr,
                "LEASE_GETFB2 REFUSE: GETFB2 failed plane=%u fb=%u errno=%d (%s)\n",
                expected_plane, plane.fb_id, errno, strerror(errno));
        if (classify)
            puts("LEASE_GETFB2 result=INDETERMINATE reason=getfb2-failed");
        close(fd);
        return 78;
    }
    printf("LEASE_GETFB2 plane=%u crtc=%u fb=%u size=%ux%u format=0x%08x modifier=0x%016llx\n",
           expected_plane, plane.crtc_id, fb.fb_id, fb.width, fb.height,
           fb.pixel_format, (unsigned long long)fb.modifiers[0]);
    close_returned_handles(fd, &fb);
    close(fd);

    if (fb.pixel_format == DRM_FORMAT_XBGR8888 &&
        fb.modifiers[0] == DRM_FORMAT_MOD_QCOM_COMPRESSED) {
        if (classify)
            puts("LEASE_GETFB2 result=UBWC_PASS exact=XB24/0x0500000000000001");
        else
            puts("LEASE_GETFB2 result=PASS exact=XB24/0x0500000000000001");
        return 0;
    }
    if (classify && fb.modifiers[0] == 0) {
        printf("LEASE_GETFB2 result=LINEAR_FALLBACK format=0x%08x modifier=0x%016llx\n",
               fb.pixel_format, (unsigned long long)fb.modifiers[0]);
        return 0;
    }
    if (classify) {
        printf("LEASE_GETFB2 result=INDETERMINATE reason=unexpected-format-modifier "
               "format=0x%08x modifier=0x%016llx\n",
               fb.pixel_format, (unsigned long long)fb.modifiers[0]);
        return 78;
    } else {
        fprintf(stderr,
                "LEASE_GETFB2 REFUSE: active framebuffer is not exact XB24/QCOM compressed\n");
        return 78;
    }
}
