#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=$root/src/bridge
host_sysroot=${BRIDGE_TEST_SYSROOT:-$root/work/bridge-host-deps/sysroot}
cc=${CC:-cc}

die() {
	printf '%s\n' "android DRM bridge framebuffer cleanup test: $*" >&2
	exit 1
}

command -v "$cc" >/dev/null 2>&1 || {
	printf '%s\n' 'android DRM bridge framebuffer cleanup test: SKIP (no compiler)'
	exit 0
}
test -r "$host_sysroot/usr/include/libdrm/drm.h" || {
	printf '%s\n' 'android DRM bridge framebuffer cleanup test: SKIP (no isolated libdrm headers)'
	exit 0
}

tmp=${TMPDIR:-/tmp}/android-drm-bridge-fb-cleanup.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

cat >"$tmp/fixture.c" <<'EOF'
#include "android_drm_bridge.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>

static unsigned int rmfb_attempts;
static unsigned int gem_close_count;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	void *argument;
	(void)fd;
	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);

	if (request == DRM_IOCTL_VERSION ||
	    request == DRM_IOCTL_MODE_GETRESOURCES)
		return 0;
	if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
		struct drm_prime_handle *prime = argument;
		prime->handle = 41;
		return 0;
	}
	if (request == DRM_IOCTL_MODE_ADDFB2) {
		struct drm_mode_fb_cmd2 *command = argument;
		command->fb_id = 73;
		return 0;
	}
	if (request == DRM_IOCTL_MODE_RMFB) {
		++rmfb_attempts;
		if (rmfb_attempts == 1) {
			errno = EBUSY;
			return -1;
		}
		return 0;
	}
	if (request == DRM_IOCTL_GEM_CLOSE) {
		++gem_close_count;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

int main(void)
{
	struct adbr_dmabuf_v1 dmabuf;
	struct adbr_fb *fb = NULL;
	int kms_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	int dma_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

	assert(kms_fd >= 0 && dma_fd >= 0);
	memset(&dmabuf, 0, sizeof(dmabuf));
	dmabuf.struct_size = sizeof(dmabuf);
	dmabuf.abi_version = ADBR_ABI_VERSION_1;
	dmabuf.width = 64;
	dmabuf.height = 64;
	dmabuf.fourcc = UINT32_C(0x34324258);
	dmabuf.plane_count = 1;
	dmabuf.planes[0].fd = dma_fd;
	dmabuf.planes[0].stride = 256;
	dmabuf.planes[0].modifier = ADBR_MODIFIER_QCOM_COMPRESSED;

	assert(adbr_fb_import_v1(kms_fd, &dmabuf, 0, &fb) == 0);
	assert(fb != NULL && adbr_fb_get_id_v1(fb) == 73);
	assert(adbr_fb_destroy_v1(fb) == -EBUSY);
	assert(adbr_fb_get_id_v1(fb) == 73);
	assert(rmfb_attempts == 1 && gem_close_count == 0);
	assert(adbr_fb_destroy_v1(fb) == 0);
	assert(rmfb_attempts == 2 && gem_close_count == 1);

	close(dma_fd);
	close(kms_fd);
	return 0;
}
EOF

"$cc" -std=gnu11 -Wall -Wextra -Werror \
	-I"$source_dir" -I"$host_sysroot/usr/include" \
	-I"$host_sysroot/usr/include/libdrm" \
	"$source_dir/android_drm_bridge.c" "$tmp/fixture.c" \
	-Wl,--wrap=ioctl -o "$tmp/fixture" || die 'fixture compilation failed'
"$tmp/fixture" || die 'RMFB retry ownership contract failed'

printf '%s\n' 'android DRM bridge framebuffer cleanup test: PASS'
