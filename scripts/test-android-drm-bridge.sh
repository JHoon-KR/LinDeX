#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_script=$root/scripts/build-android-drm-bridge.sh
cleanup_test=$root/scripts/test-android-drm-bridge-fb-cleanup.sh
probe_test=$root/scripts/test-android-drm-bridge-probe.sh
doc=$root/docs/ARCHITECTURE.md
source_dir=$root/src/bridge

die() {
	printf '%s\n' "android DRM bridge test: $*" >&2
	exit 1
}

require_text() {
	pattern=$1
	file=$2
	grep -Fq -- "$pattern" "$file" || die "missing invariant in $file: $pattern"
}

test -r "$build_script" || die "missing build script"
test -r "$cleanup_test" || die "missing framebuffer cleanup test"
test -r "$probe_test" || die "missing read-only probe test"
test -r "$doc" || die "missing documentation"
/bin/sh -n "$build_script"
/bin/sh -n "$cleanup_test"
/bin/sh -n "$probe_test"
/bin/sh -n "$0"

for source in android_drm_bridge.c android_drm_bridge.h android_drm_bridge.map \
	android_drm_blob.c android_drm_blob.h android_drm_preload.c \
	android_vulkan_drm_identity_policy.c android_vulkan_drm_identity_policy.h \
	android_vulkan_drm_identity_preload.c android_vulkan_drm_identity.map; do
	test -r "$source_dir/$source" || die "missing bridge source: $source_dir/$source"
done
require_text 'adbr_preload_validate_plane_blob_v1' "$source_dir/android_drm_bridge.c"
require_text 'adbr_preload_validate_plane_blob_v1' "$source_dir/android_drm_bridge.h"
require_text 'adbr_preload_validate_plane_blob_v1' "$source_dir/android_drm_bridge.map"
require_text 'dlopen("libandroid-drm-bridge.so.1"' \
	"$source_dir/android_drm_preload.c"
require_text 'dlopen("libdrm.so.2"' "$source_dir/android_drm_preload.c"
require_text 'dlinfo(candidate_libdrm, RTLD_DI_LINKMAP, &libdrm_map)' \
	"$source_dir/android_drm_preload.c"
require_text 'dlsym(candidate_libdrm, "drmModeGetPropertyBlob")' \
	"$source_dir/android_drm_preload.c"
require_text 'get_symbol.function == drmModeGetPropertyBlob' \
	"$source_dir/android_drm_preload.c"
require_text 'RTLD_NOLOAD' "$source_dir/android_drm_preload.c"
require_text 'dlsym(candidate_core,' "$source_dir/android_drm_preload.c"
require_text '"adbr_preload_validate_plane_blob_v1")' \
	"$source_dir/android_drm_preload.c"
require_text 'ANDROID_DRM_PRELOAD_CANDIDATE_ACK' \
	"$source_dir/android_drm_preload.c"
require_text 'ANDROID_DRM_PRELOAD_POLICY' \
	"$source_dir/android_drm_preload.c"
require_text 'ANDROID_DRM_PRELOAD_STRICT_ACK' \
	"$source_dir/android_drm_preload.c"
require_text 'exact-device-strict-xb24-qcom-no-fallback' \
	"$source_dir/android_drm_preload.c"

# Public safety and packaging claims are contract, not optional prose.
require_text 'unmodified Wayland compositor' "$doc"
require_text 'preload-only' "$doc"
require_text 'fails closed' "$doc"
require_text 'does not by itself prove direct scanout' "$doc"

tmp=${TMPDIR:-/tmp}/android-drm-bridge-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin" "$tmp/empty-source" "$tmp/mock-include/libdrm" \
	"$tmp/mock-lib" "$tmp/mock-out" "$tmp/sysroot/usr/include/libdrm" \
	"$tmp/sysroot/usr/lib/x86_64-linux-gnu"

# The mock compiler records the complete build contract and creates non-empty
# placeholders. It lets this test cover native pkg-config and cross-compiler
# overrides on hosts that cannot execute the target architecture.
cat >"$tmp/bin/mock-cc" <<'EOF'
#!/bin/sh
set -eu
: "${MOCK_CC_LOG:?}"
printf '%s\n' "$*" >>"$MOCK_CC_LOG"
out=
while test "$#" -gt 0; do
	if test "$1" = -o; then
		shift
		out=$1
		break
	fi
	shift
done
test -n "$out"
printf '%s\n' mock-elf >"$out"
EOF
cat >"$tmp/bin/mock-pkg-config" <<EOF
#!/bin/sh
set -eu
printf '%s\n' "\$*" >>"$tmp/pkg-config.log"
case "\${1:-}" in
	--exists) exit 0 ;;
	--cflags) printf '%s\n' '-I$tmp/mock-include -I$tmp/mock-include/libdrm' ;;
	--libs) printf '%s\n' '-L$tmp/mock-lib -ldrm' ;;
	*) exit 2 ;;
esac
EOF
chmod +x "$tmp/bin/mock-cc" "$tmp/bin/mock-pkg-config"

MOCK_CC_LOG=$tmp/cc.log OUT=$tmp/mock-out CC=$tmp/bin/mock-cc \
	PKG_CONFIG=$tmp/bin/mock-pkg-config "$build_script" >/dev/null
test "$(wc -l <"$tmp/cc.log")" -eq 5 || die "expected exactly five compiler invocations"
require_text '-Wl,-soname,libandroid-drm-bridge.so.1' "$tmp/cc.log"
require_text "--version-script=$source_dir/android_drm_bridge.map" "$tmp/cc.log"
require_text "$source_dir/android_drm_bridge.c" "$tmp/cc.log"
require_text "$source_dir/android_drm_preload.c" "$tmp/cc.log"
require_text "$source_dir/android_drm_blob.c" "$tmp/cc.log"
require_text '-Wl,-soname,libandroid-vulkan-drm-identity.so' "$tmp/cc.log"
require_text "--version-script=$source_dir/android_vulkan_drm_identity.map" "$tmp/cc.log"
require_text '-Wl,-soname,libandroid-vulkan-drm-identity-layer.so' "$tmp/cc.log"
require_text "--version-script=$source_dir/android_vulkan_drm_identity_layer.map" "$tmp/cc.log"
require_text '-Wl,-Bsymbolic-functions' "$tmp/cc.log"
require_text '-ldrm' "$tmp/cc.log"
test -L "$tmp/mock-out/libandroid-drm-bridge.so" || die "missing core linker-name symlink"

# BRIDGE_SYSROOT must bypass pkg-config and use the bounded include/library
# locations. BRIDGE_LIBDIR keeps AArch64 or other cross sysroots explicit.
: >"$tmp/sysroot/usr/include/xf86drm.h"
: >"$tmp/sysroot/usr/include/libdrm/drm.h"
mkdir -p "$tmp/sysroot/usr/include/vulkan"
: >"$tmp/sysroot/usr/include/vulkan/vulkan_core.h"
: >"$tmp/sysroot/usr/lib/x86_64-linux-gnu/libdrm.so"
: >"$tmp/cc-sysroot.log"
MOCK_CC_LOG=$tmp/cc-sysroot.log OUT=$tmp/sysroot-out CC=$tmp/bin/mock-cc \
	PKG_CONFIG=$tmp/bin/does-not-exist BRIDGE_SYSROOT=$tmp/sysroot \
	"$build_script" >/dev/null
require_text "-I$tmp/sysroot/usr/include" "$tmp/cc-sysroot.log"
require_text "-I$tmp/sysroot/usr/include/libdrm" "$tmp/cc-sysroot.log"
require_text "-L$tmp/sysroot/usr/lib/x86_64-linux-gnu" "$tmp/cc-sysroot.log"

# Missing sources, dependency metadata, or sysroot headers must never produce
# a partial/successful build.
if MOCK_CC_LOG=$tmp/reject.log CC=$tmp/bin/mock-cc \
	BRIDGE_SOURCE_DIR=$tmp/empty-source "$build_script" >/dev/null 2>&1; then
	die "build accepted a missing source tree"
fi
if MOCK_CC_LOG=$tmp/reject.log CC=$tmp/bin/mock-cc \
	PKG_CONFIG=$tmp/bin/does-not-exist "$build_script" >/dev/null 2>&1; then
	die "build accepted missing pkg-config dependency metadata"
fi
if MOCK_CC_LOG=$tmp/reject.log CC=$tmp/bin/mock-cc \
	BRIDGE_SYSROOT=$tmp/empty-source "$build_script" >/dev/null 2>&1; then
	die "build accepted an incomplete sysroot"
fi

# If a usable native compiler and isolated host sysroot are available, also
# perform a real link and inspect the dynamic ABI. This is deliberately an
# opt-in/detected host check, never a substitute for target-device validation.
host_sysroot=${BRIDGE_TEST_SYSROOT:-$root/work/bridge-host-deps/sysroot}
if command -v "${CC:-cc}" >/dev/null 2>&1 && \
	test -r "$host_sysroot/usr/include/xf86drm.h" && \
	test -r "$host_sysroot/usr/include/vulkan/vulkan_core.h" && \
	test -r "$host_sysroot/usr/lib/x86_64-linux-gnu/libdrm.so"; then
	host_cc=${CC:-cc}
	host_includes="-I$host_sysroot/usr/include -I$host_sysroot/usr/include/libdrm"
	host_libdir=$host_sysroot/usr/lib/x86_64-linux-gnu
	OUT=$tmp/real-out BRIDGE_SYSROOT=$host_sysroot CC=${CC:-cc} \
		"$build_script" >/dev/null
	test -s "$tmp/real-out/libandroid-drm-bridge.so.1"
	test -s "$tmp/real-out/libandroid-drm-preload.so"
	test -s "$tmp/real-out/libandroid-vulkan-drm-identity.so"
	if command -v readelf >/dev/null 2>&1; then
		readelf -d "$tmp/real-out/libandroid-drm-bridge.so.1" |
			grep -Fq 'libandroid-drm-bridge.so.1' || die "core SONAME mismatch"
	fi
	if command -v nm >/dev/null 2>&1; then
		preload_symbols=$tmp/preload-symbols
		nm -D --defined-only "$tmp/real-out/libandroid-drm-preload.so" >"$preload_symbols"
		require_text 'drmModeGetPropertyBlob' "$preload_symbols"
		require_text 'drmModeFreePropertyBlob' "$preload_symbols"
		if grep -Fq 'android_drm_blob_append_xb24_qcom' "$preload_symbols"; then
			die "preload exports its private blob helper"
		fi
	fi

	# Reproduce the loader topology that caused the reference-device Vulkan
	# crash: an earlier preload wraps dlsym() and forwards RTLD_NEXT from its
	# own call site. The DRM frontend must still resolve the exact libdrm
	# implementation instead of storing its own wrapper and recursing.
	cat >"$tmp/dlsym-forwarder.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

typedef void *(*dlsym_fn)(void *, const char *);

void *dlsym(void *handle, const char *name)
{
	static dlsym_fn real_dlsym;

	if (real_dlsym == NULL) {
		real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
		if (real_dlsym == NULL) {
			real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
		}
	}
	return real_dlsym == NULL ? NULL : real_dlsym(handle, name);
}
EOF
	cat >"$tmp/preload-recursion-fixture.c" <<'EOF'
#include <errno.h>
#include <xf86drmMode.h>

int main(void)
{
	drmModePropertyBlobPtr blob;

	errno = 0;
	blob = drmModeGetPropertyBlob(-1, 1);
	if (blob != NULL) {
		drmModeFreePropertyBlob(blob);
		return 1;
	}
	return 0;
}
EOF
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror -fPIC -shared \
		"$tmp/dlsym-forwarder.c" -ldl -o "$tmp/libdlsym-forwarder.so"
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror $host_includes \
		"$tmp/preload-recursion-fixture.c" -L"$host_libdir" -ldrm \
		-Wl,-rpath,"$host_libdir" -o "$tmp/preload-recursion-fixture"
	LD_LIBRARY_PATH="$tmp/real-out:$host_libdir" \
	LD_PRELOAD="$tmp/libdlsym-forwarder.so:$tmp/real-out/libandroid-drm-preload.so" \
		timeout 5 "$tmp/preload-recursion-fixture" ||
		die "DRM preload recursed behind an earlier dlsym interposer"

	# Exercise the stable core ABI and all descriptor-only rejection paths
	# without pretending that a harmless host character device is a DRM lease.
	cat >"$tmp/core-fixture.c" <<'EOF'
#include "android_drm_bridge.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define XB24 UINT32_C(0x34324258)

int main(void)
{
	struct adbr_dmabuf_v1 dmabuf;
	struct adbr_lease_info_v1 lease_info = {
		.struct_size = sizeof(lease_info),
		.abi_version = ADBR_ABI_VERSION_1,
	};
	struct adbr_fb_observation_v1 observation = {
		.struct_size = sizeof(observation),
		.abi_version = ADBR_ABI_VERSION_1,
	};
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int owned_fd = -1;
	uint64_t caps;

	assert(fd >= 0);
	assert(adbr_get_abi_version() == ADBR_ABI_VERSION_1);
	caps = adbr_get_capabilities();
	assert((caps & (ADBR_CAP_LEASE_VALIDATE | ADBR_CAP_PLANE_FORMATS |
	                ADBR_CAP_QCOM_CANDIDATES | ADBR_CAP_PRIME_FB_IMPORT |
	                ADBR_CAP_GETFB2_OBSERVE | ADBR_CAP_PRELOAD_VALIDATE)) ==
	       (ADBR_CAP_LEASE_VALIDATE | ADBR_CAP_PLANE_FORMATS |
	        ADBR_CAP_QCOM_CANDIDATES | ADBR_CAP_PRIME_FB_IMPORT |
	        ADBR_CAP_GETFB2_OBSERVE | ADBR_CAP_PRELOAD_VALIDATE));

	memset(&dmabuf, 0, sizeof(dmabuf));
	dmabuf.struct_size = sizeof(dmabuf);
	dmabuf.abi_version = ADBR_ABI_VERSION_1;
	dmabuf.width = 64;
	dmabuf.height = 64;
	dmabuf.fourcc = XB24;
	dmabuf.plane_count = 1;
	dmabuf.planes[0].fd = fd;
	dmabuf.planes[0].stride = 256;
	dmabuf.planes[0].modifier = ADBR_MODIFIER_LINEAR;
	assert(adbr_dmabuf_validate_v1(&dmabuf) == 0);
	dmabuf.planes[0].modifier = ADBR_MODIFIER_INVALID;
	assert(adbr_dmabuf_validate_v1(&dmabuf) == -EINVAL);
	dmabuf.planes[0].modifier = ADBR_MODIFIER_LINEAR;
	dmabuf.planes[1].stride = 1;
	assert(adbr_dmabuf_validate_v1(&dmabuf) == -EINVAL);

	assert(adbr_lease_dup_validate_v1(fd, NULL, 0, 0, &owned_fd,
	                                  &lease_info) < 0);
	assert(owned_fd == -1);
	assert(adbr_preload_validate_plane_blob_v1(
	           fd, 1, 1, XB24, ADBR_MODIFIER_QCOM_COMPRESSED) < 0);
	assert(adbr_fb_observe_v1(fd, 1, &observation) < 0);
	close(fd);
	return 0;
}
EOF
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror \
		-I"$source_dir" "$tmp/core-fixture.c" \
		-L"$tmp/real-out" -landroid-drm-bridge \
		-Wl,-rpath,"$tmp/real-out" -o "$tmp/core-fixture"
	LD_LIBRARY_PATH=$tmp/real-out "$tmp/core-fixture" ||
		die "core ABI/descriptor fixture failed"

	cat >"$tmp/blob-negative.c" <<'EOF'
#include "android_drm_blob.h"
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct one_blob {
	struct drm_format_modifier_blob header;
	uint32_t format;
	uint32_t padding;
	struct drm_format_modifier modifier;
};

struct two_format_blob {
	struct drm_format_modifier_blob header;
	uint32_t formats[2];
	struct drm_format_modifier modifier;
};

struct two_modifier_blob {
	struct drm_format_modifier_blob header;
	uint32_t format;
	uint32_t padding;
	struct drm_format_modifier modifiers[2];
};

static void init_one(struct one_blob *blob)
{
	memset(blob, 0, sizeof(*blob));
	blob->header.version = 1;
	blob->header.count_formats = 1;
	blob->header.formats_offset = offsetof(struct one_blob, format);
	blob->header.count_modifiers = 1;
	blob->header.modifiers_offset = offsetof(struct one_blob, modifier);
	blob->format = DRM_FORMAT_XBGR8888;
	blob->modifier.formats = 1;
	blob->modifier.modifier = DRM_FORMAT_MOD_LINEAR;
}

static void rejected(const void *data, size_t length)
{
	void *output = (void *)(uintptr_t)1;
	size_t output_length = 1;
	assert(android_drm_blob_append_xb24_qcom(data, length, &output,
	                                        &output_length) == 0);
	assert(output == NULL && output_length == 0);
}

int main(void)
{
	struct one_blob one, original;
	struct two_format_blob duplicate;
	struct two_modifier_blob present;
	void *output = NULL;
	size_t output_length = 0;

	init_one(&one);
	rejected(&one, sizeof(one.header) - 1);
	rejected(&one, sizeof(one) - 1);
	one.header.version = 2;
	rejected(&one, sizeof(one));
	init_one(&one); one.header.flags = 1; rejected(&one, sizeof(one));
	init_one(&one); one.header.modifiers_offset = one.header.formats_offset;
	rejected(&one, sizeof(one));
	init_one(&one); one.modifier.formats = 2; rejected(&one, sizeof(one));

	memset(&duplicate, 0, sizeof(duplicate));
	duplicate.header.version = 1;
	duplicate.header.count_formats = 2;
	duplicate.header.formats_offset = offsetof(struct two_format_blob, formats);
	duplicate.header.count_modifiers = 1;
	duplicate.header.modifiers_offset = offsetof(struct two_format_blob, modifier);
	duplicate.formats[0] = DRM_FORMAT_XBGR8888;
	duplicate.formats[1] = DRM_FORMAT_XBGR8888;
	duplicate.modifier.formats = 3;
	duplicate.modifier.modifier = DRM_FORMAT_MOD_LINEAR;
	rejected(&duplicate, sizeof(duplicate));

	memset(&present, 0, sizeof(present));
	present.header.version = 1;
	present.header.count_formats = 1;
	present.header.formats_offset = offsetof(struct two_modifier_blob, format);
	present.header.count_modifiers = 2;
	present.header.modifiers_offset = offsetof(struct two_modifier_blob, modifiers);
	present.format = DRM_FORMAT_XBGR8888;
	present.modifiers[0].formats = present.modifiers[1].formats = 1;
	present.modifiers[0].modifier = DRM_FORMAT_MOD_LINEAR;
	present.modifiers[1].modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
	rejected(&present, sizeof(present));

	init_one(&one);
	memcpy(&original, &one, sizeof(one));
	assert(android_drm_blob_append_xb24_qcom(&one, sizeof(one), &output,
	                                        &output_length) == 1);
	assert(memcmp(&one, &original, sizeof(one)) == 0);
	assert(output_length == sizeof(one) + sizeof(struct drm_format_modifier));
	free(output);
	return 0;
}
EOF
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror $host_includes \
		-I"$source_dir" "$source_dir/android_drm_blob.c" \
		"$tmp/blob-negative.c" -o "$tmp/blob-negative"
	"$tmp/blob-negative" || die "malformed blob rejection fixture failed"
	if "$host_cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
		-fsanitize=address,undefined -fno-omit-frame-pointer $host_includes \
		-I"$source_dir" "$source_dir/android_drm_blob.c" \
		"$tmp/blob-negative.c" -o "$tmp/blob-negative-sanitized" \
		>/dev/null 2>&1; then
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 \
			"$tmp/blob-negative-sanitized" ||
			die "sanitized malformed blob fixture failed"
	fi

	# Exercise the preload frontend's callback contract and both ownership
	# branches against a deterministic fake libdrm. The accepted path must free
	# the replaced original exactly once and must not forward the synthetic
	# blob to libdrm; pass-through ownership must still be forwarded.
	cat >"$tmp/fake-drm.c" <<'EOF'
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drmMode.h>

	static unsigned int free_count;

	struct fixture_blob {
		struct drm_format_modifier_blob header;
		uint32_t format;
		uint32_t padding;
		struct drm_format_modifier modifier;
	};

	struct duplicate_blob {
		struct drm_format_modifier_blob header;
		uint32_t formats[2];
		struct drm_format_modifier modifier;
	};

	struct qcom_blob {
		struct drm_format_modifier_blob header;
		uint32_t format;
		uint32_t padding;
		struct drm_format_modifier modifiers[2];
	};

	static drmModePropertyBlobPtr allocate_blob(uint32_t blob_id, size_t length)
	{
		drmModePropertyBlobPtr blob = calloc(1, sizeof(*blob));

		if (blob == NULL)
			return NULL;
		blob->data = calloc(1, length);
		if (blob->data == NULL) {
			free(blob);
			return NULL;
		}
		blob->id = blob_id;
		blob->length = (uint32_t)length;
		return blob;
	}

	drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
	{
		const char *mode = getenv("FIXTURE_BLOB_MODE");
		struct fixture_blob *data;
		drmModePropertyBlobPtr blob;
		(void)fd;

		if (mode != NULL && strcmp(mode, "duplicate") == 0) {
			struct duplicate_blob *duplicate;

			blob = allocate_blob(blob_id, sizeof(*duplicate));
			if (blob == NULL)
				return NULL;
			duplicate = blob->data;
			duplicate->header.version = 1;
			duplicate->header.count_formats = 2;
			duplicate->header.formats_offset =
				offsetof(struct duplicate_blob, formats);
			duplicate->header.count_modifiers = 1;
			duplicate->header.modifiers_offset =
				offsetof(struct duplicate_blob, modifier);
			duplicate->formats[0] = DRM_FORMAT_XBGR8888;
			duplicate->formats[1] = DRM_FORMAT_XBGR8888;
			duplicate->modifier.formats = 3;
			duplicate->modifier.modifier = DRM_FORMAT_MOD_LINEAR;
			return blob;
		}
		if (mode != NULL && strcmp(mode, "already-qcom") == 0) {
			struct qcom_blob *qcom;

			blob = allocate_blob(blob_id, sizeof(*qcom));
			if (blob == NULL)
				return NULL;
			qcom = blob->data;
			qcom->header.version = 1;
			qcom->header.count_formats = 1;
			qcom->header.formats_offset =
				offsetof(struct qcom_blob, format);
			qcom->header.count_modifiers = 2;
			qcom->header.modifiers_offset =
				offsetof(struct qcom_blob, modifiers);
			qcom->format = DRM_FORMAT_XBGR8888;
			qcom->modifiers[0].formats = 1;
			qcom->modifiers[0].modifier = DRM_FORMAT_MOD_LINEAR;
			qcom->modifiers[1].formats = 1;
			qcom->modifiers[1].modifier =
				DRM_FORMAT_MOD_QCOM_COMPRESSED;
			return blob;
		}

		blob = allocate_blob(blob_id, sizeof(*data));
		if (blob == NULL)
			return NULL;
		data = blob->data;
		data->header.version = 1;
		if (mode != NULL && strcmp(mode, "malformed") == 0)
			data->header.version = 2;
		data->header.count_formats = 1;
	data->header.formats_offset = offsetof(struct fixture_blob, format);
	data->header.count_modifiers = 1;
	data->header.modifiers_offset = offsetof(struct fixture_blob, modifier);
	data->format = DRM_FORMAT_XBGR8888;
	data->modifier.formats = 1;
	data->modifier.modifier = DRM_FORMAT_MOD_LINEAR;
		return blob;
	}

void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob)
{
	if (blob != NULL) {
		++free_count;
		free(blob->data);
		free(blob);
	}
}

unsigned int fake_drm_free_count(void)
{
	return free_count;
}
EOF
	cat >"$tmp/fake-core.c" <<'EOF'
#include "android_drm_bridge.h"
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdlib.h>

uint32_t adbr_get_abi_version(void)
{
	return ADBR_ABI_VERSION_1;
}

uint64_t adbr_get_capabilities(void)
{
	if (getenv("FIXTURE_DISABLE_PRELOAD_CAP") != NULL)
		return ADBR_CAP_PLANE_FORMATS | ADBR_CAP_QCOM_CANDIDATES;
	return ADBR_CAP_PLANE_FORMATS | ADBR_CAP_QCOM_CANDIDATES |
	       ADBR_CAP_PRELOAD_VALIDATE;
}

int adbr_preload_validate_plane_blob_v1(int fd, uint32_t plane,
	uint32_t blob, uint32_t fourcc, uint64_t modifier)
{
	return fd == 9 && plane == 17 && blob == 23 &&
		fourcc == DRM_FORMAT_XBGR8888 &&
		modifier == ADBR_MODIFIER_QCOM_COMPRESSED ? 0 : -1;
}
EOF
	cat >"$tmp/preload-fixture.c" <<'EOF'
#include "android_drm_bridge.h"
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drmMode.h>

#ifndef DRM_FORMAT_MOD_QCOM_COMPRESSED
#define DRM_FORMAT_MOD_QCOM_COMPRESSED fourcc_mod_code(QCOM, 1)
#endif

unsigned int fake_drm_free_count(void);

struct expected_strict_blob {
	struct drm_format_modifier_blob header;
	uint32_t format;
	uint32_t padding;
	struct drm_format_modifier modifier;
};

struct expected_strict_fallback_blob {
	struct drm_format_modifier_blob header;
	uint32_t format;
	uint32_t padding;
	struct drm_format_modifier modifiers[2];
};

static void set_base(void)
{
	assert(setenv("ANDROID_DRM_PRELOAD_ENABLE", "1", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_PRIMARY_PLANE", "17", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "23", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK",
	              "exact-device-candidate-only", 1) == 0);
}

static void set_strict(void)
{
	set_base();
	assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "strict-xb24-qcom", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_STRICT_ACK",
	              "exact-device-strict-xb24-qcom-no-fallback", 1) == 0);
}

static void set_strict_fallback(void)
{
	set_base();
	assert(setenv("ANDROID_DRM_PRELOAD_POLICY",
	              "strict-xb24-qcom-linear", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_STRICT_ACK",
	              "exact-device-strict-xb24-qcom-linear-fallback", 1) == 0);
}

static void assert_original(drmModePropertyBlobPtr blob)
{
	const struct drm_format_modifier_blob *header;
	const uint32_t *formats;
	const struct drm_format_modifier *modifiers;

	assert(blob != NULL && blob->id != 0);
	header = blob->data;
	assert(header->version == 1 && header->flags == 0);
	assert(header->count_formats == 1 && header->count_modifiers == 1);
	formats = (const void *)((const unsigned char *)blob->data +
	                         header->formats_offset);
	modifiers = (const void *)((const unsigned char *)blob->data +
	                           header->modifiers_offset);
	assert(formats[0] == DRM_FORMAT_XBGR8888);
	assert(modifiers[0].formats == 1);
	assert(modifiers[0].modifier == DRM_FORMAT_MOD_LINEAR);
}

static void assert_strict(drmModePropertyBlobPtr blob)
{
	const struct expected_strict_blob *strict;

	assert(blob != NULL && blob->id == 23);
	assert(blob->length == sizeof(*strict));
	strict = blob->data;
	assert(strict->header.version == 1 && strict->header.flags == 0);
	assert(strict->header.count_formats == 1);
	assert(strict->header.formats_offset ==
	       offsetof(struct expected_strict_blob, format));
	assert(strict->header.count_modifiers == 1);
	assert(strict->header.modifiers_offset ==
	       offsetof(struct expected_strict_blob, modifier));
	assert(strict->format == DRM_FORMAT_XBGR8888);
	assert(strict->padding == 0);
	assert(strict->modifier.formats == 1);
	assert(strict->modifier.offset == 0);
	assert(strict->modifier.pad == 0);
	assert(strict->modifier.modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED);
}

static void assert_strict_fallback(drmModePropertyBlobPtr blob)
{
	const struct expected_strict_fallback_blob *strict;

	assert(blob != NULL && blob->id == 23);
	assert(blob->length == sizeof(*strict));
	strict = blob->data;
	assert(strict->header.version == 1 && strict->header.flags == 0);
	assert(strict->header.count_formats == 1);
	assert(strict->header.formats_offset ==
	       offsetof(struct expected_strict_fallback_blob, format));
	assert(strict->header.count_modifiers == 2);
	assert(strict->header.modifiers_offset ==
	       offsetof(struct expected_strict_fallback_blob, modifiers));
	assert(strict->format == DRM_FORMAT_XBGR8888);
	assert(strict->padding == 0);
	assert(strict->modifiers[0].formats == 1);
	assert(strict->modifiers[0].offset == 0);
	assert(strict->modifiers[0].modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED);
	assert(strict->modifiers[1].formats == 1);
	assert(strict->modifiers[1].offset == 0);
	assert(strict->modifiers[1].modifier == DRM_FORMAT_MOD_LINEAR);
}

int main(int argc, char **argv)
{
	drmModePropertyBlobPtr blob;
	const struct drm_format_modifier_blob *header;
	const struct drm_format_modifier *modifiers;
	unsigned int index;

	if (argc == 2 && strcmp(argv[1], "invalid-ack-only") == 0) {
		set_base();
		assert(setenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK", "invalid", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);
		assert(fake_drm_free_count() == 1);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "strict-invalid-ack-only") == 0) {
		set_base();
		assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "strict-xb24-qcom", 1) == 0);
		assert(unsetenv("ANDROID_DRM_PRELOAD_STRICT_ACK") == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);
		assert(setenv("ANDROID_DRM_PRELOAD_STRICT_ACK", "invalid", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);
		assert(fake_drm_free_count() == 2);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "strict-negative-only") == 0) {
		set_strict();

		blob = drmModeGetPropertyBlob(8, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);

		assert(setenv("ANDROID_DRM_PRELOAD_PRIMARY_PLANE", "18", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);

		assert(setenv("ANDROID_DRM_PRELOAD_PRIMARY_PLANE", "17", 1) == 0);
		assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "24", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);

		assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "auto", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 24);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);

		assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "23", 1) == 0);
		assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "unknown", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert_original(blob);
		drmModeFreePropertyBlob(blob);

		assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "strict-xb24-qcom", 1) == 0);
		assert(setenv("FIXTURE_BLOB_MODE", "malformed", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert(blob != NULL);
		assert(((const struct drm_format_modifier_blob *)blob->data)->version == 2);
		drmModeFreePropertyBlob(blob);

		assert(setenv("FIXTURE_BLOB_MODE", "duplicate", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert(blob != NULL);
		assert(((const struct drm_format_modifier_blob *)blob->data)->count_formats == 2);
		drmModeFreePropertyBlob(blob);

		assert(setenv("FIXTURE_BLOB_MODE", "already-qcom", 1) == 0);
		blob = drmModeGetPropertyBlob(9, 23);
		assert(blob != NULL);
		assert(((const struct drm_format_modifier_blob *)blob->data)->count_modifiers == 2);
		drmModeFreePropertyBlob(blob);
		assert(fake_drm_free_count() == 8);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "strict-success-only") == 0) {
		set_strict();
		for (index = 0; index < 2; ++index) {
			blob = drmModeGetPropertyBlob(9, 23);
			assert_strict(blob);
			assert(fake_drm_free_count() == index + 1);
			drmModeFreePropertyBlob(blob);
			assert(fake_drm_free_count() == index + 1);
		}
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "strict-fallback-success-only") == 0) {
		set_strict_fallback();
		for (index = 0; index < 2; ++index) {
			blob = drmModeGetPropertyBlob(9, 23);
			assert_strict_fallback(blob);
			assert(fake_drm_free_count() == index + 1);
			drmModeFreePropertyBlob(blob);
			assert(fake_drm_free_count() == index + 1);
		}
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "strict-lifetime-only") == 0) {
		drmModePropertyBlobPtr blobs[65];

		set_strict();
		for (index = 0; index < 64; ++index) {
			blobs[index] = drmModeGetPropertyBlob(9, 23);
			assert_strict(blobs[index]);
			assert(fake_drm_free_count() == index + 1);
		}
		blobs[64] = drmModeGetPropertyBlob(9, 23);
		assert_original(blobs[64]);
		assert(fake_drm_free_count() == 64);
		drmModeFreePropertyBlob(blobs[64]);
		assert(fake_drm_free_count() == 65);
		for (index = 64; index > 0; --index)
			drmModeFreePropertyBlob(blobs[index - 1]);
		assert(fake_drm_free_count() == 65);
		return 0;
	}
	assert(argc == 1);
	set_base();
	assert(unsetenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK") == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(((const struct drm_format_modifier_blob *)blob->data)->count_modifiers == 1);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 1);

	assert(setenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK", "this-is-proof", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(((const struct drm_format_modifier_blob *)blob->data)->count_modifiers == 1);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 2);

	assert(setenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK",
	              "exact-device-candidate-only", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "append", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_STRICT_ACK", "invalid", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(fake_drm_free_count() == 3);
	header = blob->data;
	assert(header->count_modifiers == 2);
	modifiers = (const void *)((const unsigned char *)blob->data +
	                          header->modifiers_offset);
	assert(modifiers[1].modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 3);

	assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "auto", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(fake_drm_free_count() == 4);
	header = blob->data;
	assert(header->count_modifiers == 2);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 4);

	assert(setenv("FIXTURE_DISABLE_PRELOAD_CAP", "1", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(fake_drm_free_count() == 4);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 5);
	assert(unsetenv("FIXTURE_DISABLE_PRELOAD_CAP") == 0);

	assert(setenv("ANDROID_DRM_PRELOAD_ENABLE", "0", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 6);
	return 0;
}
EOF
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -fPIC -shared -Wall -Wextra -Werror \
		$host_includes "$tmp/fake-drm.c" -Wl,-soname,libdrm.so.2 \
		-o "$tmp/libdrm.so.2"
	ln -s libdrm.so.2 "$tmp/libfake-drm.so"
	"$host_cc" -std=gnu11 -fPIC -shared -Wall -Wextra -Werror \
		$host_includes -I"$source_dir" "$tmp/fake-core.c" \
		-Wl,-soname,libandroid-drm-bridge.so.1 \
		-o "$tmp/libandroid-drm-bridge.so.1"
	ln -s libandroid-drm-bridge.so.1 "$tmp/libandroid-drm-bridge.so"
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror -rdynamic \
		$host_includes -I"$source_dir" \
		"$source_dir/android_drm_preload.c" \
		"$source_dir/android_drm_blob.c" "$tmp/preload-fixture.c" \
		-L"$tmp" -Wl,--no-as-needed -landroid-drm-bridge -Wl,-l:libdrm.so.2 \
		-Wl,-rpath,"$tmp" -ldl -pthread -o "$tmp/preload-fixture"
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" invalid-ack-only \
		2>"$tmp/preload-invalid-ack.log" ||
		die "preload invalid-ack pass-through fixture failed"
	if grep -Eq '^ADBR-PRELOAD IN_FORMATS_(AUGMENTED|STRICT) ' \
		"$tmp/preload-invalid-ack.log"; then
		die "preload logged an augmentation with an invalid acknowledgement"
	fi
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" strict-invalid-ack-only \
		2>"$tmp/preload-strict-invalid-ack.log" ||
		die "preload strict invalid-ack pass-through fixture failed"
	if grep -Eq '^ADBR-PRELOAD IN_FORMATS_(AUGMENTED|STRICT) ' \
		"$tmp/preload-strict-invalid-ack.log"; then
		die "preload logged strict replacement without the exact strict acknowledgement"
	fi
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" strict-negative-only \
		2>"$tmp/preload-strict-negative.log" ||
		die "preload strict rejection fixture failed"
	if grep -Eq '^ADBR-PRELOAD IN_FORMATS_(AUGMENTED|STRICT) ' \
		"$tmp/preload-strict-negative.log"; then
		die "preload logged a replacement for a wrong target or invalid source blob"
	fi
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" strict-success-only \
		2>"$tmp/preload-strict-success.log" ||
		die "preload strict callback/ownership fixture failed"
	grep -Eq '^ADBR-PRELOAD IN_FORMATS_STRICT pid=[0-9]+ plane=17 blob=23 format=XB24 fourcc=0x34324258 modifier=0x0500000000000001 policy=exact-device-xb24-qcom-only$' \
		"$tmp/preload-strict-success.log" ||
		die "preload strict success proof log is missing or malformed"
	test "$(grep -c '^ADBR-PRELOAD IN_FORMATS_STRICT ' "$tmp/preload-strict-success.log")" -eq 1 ||
		die "preload strict success proof log was not one-shot"
	if grep -q '^ADBR-PRELOAD IN_FORMATS_AUGMENTED ' \
		"$tmp/preload-strict-success.log"; then
		die "preload strict mode emitted the append-mode proof log"
	fi
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" strict-fallback-success-only \
		2>"$tmp/preload-strict-fallback-success.log" ||
		die "preload strict fallback callback/ownership fixture failed"
	grep -Eq '^ADBR-PRELOAD IN_FORMATS_STRICT_FALLBACK pid=[0-9]+ plane=17 blob=23 format=XB24 fourcc=0x34324258 modifiers=0x0500000000000001,0x0000000000000000 policy=exact-device-xb24-qcom-preferred-linear-fallback$' \
		"$tmp/preload-strict-fallback-success.log" ||
		die "preload strict fallback success proof log is missing or malformed"
	test "$(grep -c '^ADBR-PRELOAD IN_FORMATS_STRICT_FALLBACK ' "$tmp/preload-strict-fallback-success.log")" -eq 1 ||
		die "preload strict fallback proof log was not one-shot"
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" strict-lifetime-only \
		2>"$tmp/preload-strict-lifetime.log" ||
		die "preload strict lifetime/capacity fixture failed"
	test "$(grep -c '^ADBR-PRELOAD IN_FORMATS_STRICT ' "$tmp/preload-strict-lifetime.log")" -eq 1 ||
		die "preload strict lifetime proof log was not one-shot"
	LD_LIBRARY_PATH=$tmp "$tmp/preload-fixture" 2>"$tmp/preload-fixture.log" ||
		die "preload callback/ownership fixture failed"
	grep -Eq '^ADBR-PRELOAD IN_FORMATS_AUGMENTED pid=[0-9]+ plane=17 blob=23 format=XB24 fourcc=0x34324258 modifier=0x0500000000000001 source=exact-device-candidate$' \
		"$tmp/preload-fixture.log" ||
		die "preload success proof log is missing or malformed"
	test "$(grep -c '^ADBR-PRELOAD IN_FORMATS_AUGMENTED ' "$tmp/preload-fixture.log")" -eq 1 ||
		die "preload success proof log was not one-shot"
	if grep -q '^ADBR-PRELOAD IN_FORMATS_STRICT ' "$tmp/preload-fixture.log"; then
		die "preload append mode emitted the strict-mode proof log"
	fi

	cat >"$tmp/preload-no-core.c" <<'EOF'
#include <assert.h>
#include <drm_mode.h>
#include <stdlib.h>
#include <xf86drmMode.h>

unsigned int fake_drm_free_count(void);

int main(void)
{
	drmModePropertyBlobPtr blob;
	assert(setenv("ANDROID_DRM_PRELOAD_ENABLE", "1", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_CANDIDATE_ACK",
	              "exact-device-candidate-only", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_PRIMARY_PLANE", "17", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_IN_FORMATS_BLOB", "23", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(((const struct drm_format_modifier_blob *)blob->data)->count_modifiers == 1);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 1);
	assert(setenv("ANDROID_DRM_PRELOAD_POLICY", "strict-xb24-qcom", 1) == 0);
	assert(setenv("ANDROID_DRM_PRELOAD_STRICT_ACK",
	              "exact-device-strict-xb24-qcom-no-fallback", 1) == 0);
	blob = drmModeGetPropertyBlob(9, 23);
	assert(blob != NULL);
	assert(((const struct drm_format_modifier_blob *)blob->data)->count_modifiers == 1);
	drmModeFreePropertyBlob(blob);
	assert(fake_drm_free_count() == 2);
	return 0;
}
EOF
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror \
		$host_includes -I"$source_dir" \
		"$source_dir/android_drm_preload.c" \
		"$source_dir/android_drm_blob.c" "$tmp/preload-no-core.c" \
		-L"$tmp" -Wl,--no-as-needed -Wl,-l:libdrm.so.2 \
		-Wl,-rpath,"$tmp" -ldl -pthread -o "$tmp/preload-no-core"
	LD_LIBRARY_PATH=$tmp "$tmp/preload-no-core" 2>"$tmp/preload-no-core.log" ||
		die "preload missing-core pass-through fixture failed"
	if grep -Eq '^ADBR-PRELOAD IN_FORMATS_(AUGMENTED|STRICT) ' \
		"$tmp/preload-no-core.log"; then
		die "preload logged an append or strict replacement without the common core"
	fi

	# Exercise the libseat frontend with a harmless character device and a
	# deterministic core validator. The target path must receive the validated
	# duplicate, malformed/missing object manifests must fail closed, and all
	# unrelated paths must continue to the real libseat implementation.
	cat >"$tmp/fake-seat.c" <<'EOF'
#include <fcntl.h>
#include <unistd.h>

struct libseat;
static unsigned int open_count;
static unsigned int close_count;

int libseat_open_device(struct libseat *seat, const char *path, int *fd_out)
{
	(void)seat;
	++open_count;
	*fd_out = open(path, O_RDONLY | O_CLOEXEC);
	return *fd_out < 0 ? -1 : 77;
}

int libseat_close_device(struct libseat *seat, int device_id)
{
	(void)seat;
	(void)device_id;
	++close_count;
	return 0;
}

unsigned int fake_seat_open_count(void) { return open_count; }
unsigned int fake_seat_close_count(void) { return close_count; }
EOF
	cat >"$tmp/seat-fixture.c" <<'EOF'
#include "android_drm_bridge.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct libseat;
int libseat_open_device(struct libseat *, const char *, int *);
int libseat_close_device(struct libseat *, int);
unsigned int fake_seat_open_count(void);
unsigned int fake_seat_close_count(void);

int adbr_lease_dup_validate_v1(
	int borrowed_fd, const uint32_t *expected_objects, size_t expected_count,
	uint32_t flags, int *out_owned_fd, struct adbr_lease_info_v1 *out_info)
{
	if (borrowed_fd < 0 || expected_objects == NULL || expected_count != 3 ||
	    expected_objects[0] != 11 || expected_objects[1] != 22 ||
	    expected_objects[2] != 33 || flags != 0 || out_owned_fd == NULL ||
	    out_info == NULL || out_info->struct_size != sizeof(*out_info) ||
	    out_info->abi_version != ADBR_ABI_VERSION_1)
		return -EINVAL;
	*out_owned_fd = fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 3);
	if (*out_owned_fd < 0)
		return -errno;
	out_info->object_count = 3;
	return 0;
}

int main(void)
{
	char lease_text[32];
	int lease_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int opened_fd = -1;
	int device_id;

	assert(lease_fd >= 0);
	assert(snprintf(lease_text, sizeof(lease_text), "%d", lease_fd) > 0);
	assert(setenv("DRM_LEASE_DEVICE", "/dev/null", 1) == 0);
	assert(setenv("DRM_LEASE_FD", lease_text, 1) == 0);
	assert(setenv("DRM_LEASE_OBJECTS", "11,22,33", 1) == 0);
	assert(fcntl(lease_fd, F_SETFD, 0) == 0);
	device_id = libseat_open_device(NULL, "/dev/null", &opened_fd);
	assert(device_id == 0x4c530000);
	assert(opened_fd >= 0 && opened_fd != lease_fd);
	assert((fcntl(lease_fd, F_GETFD) & FD_CLOEXEC) != 0);
	assert(fake_seat_open_count() == 0);
	assert(libseat_close_device(NULL, device_id) == 0);
	assert(fcntl(opened_fd, F_GETFD) >= 0);
	close(opened_fd);
	assert(fake_seat_close_count() == 0);

	assert(setenv("DRM_LEASE_OBJECTS", "11,22,22", 1) == 0);
	errno = 0;
	assert(libseat_open_device(NULL, "/dev/null", &opened_fd) == -1);
	assert(errno == EINVAL && fake_seat_open_count() == 0);
	assert(unsetenv("DRM_LEASE_OBJECTS") == 0);
	errno = 0;
	assert(libseat_open_device(NULL, "/dev/null", &opened_fd) == -1);
	assert(errno == EINVAL && fake_seat_open_count() == 0);

	assert(setenv("DRM_LEASE_OBJECTS", "11,22,33", 1) == 0);
	device_id = libseat_open_device(NULL, "/dev/zero", &opened_fd);
	assert(device_id == 77 && opened_fd >= 0);
	assert(fake_seat_open_count() == 1);
	close(opened_fd);
	assert(libseat_close_device(NULL, device_id) == 0);
	assert(fake_seat_close_count() == 1);

	assert(unsetenv("DRM_LEASE_FD") == 0);
	device_id = libseat_open_device(NULL, "/dev/null", &opened_fd);
	assert(device_id == 77 && opened_fd >= 0);
	assert(fake_seat_open_count() == 2);
	close(opened_fd);
	close(lease_fd);
	return 0;
}
EOF
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -fPIC -shared -Wall -Wextra -Werror \
		"$tmp/fake-seat.c" -o "$tmp/libfake-seat.so"
	# shellcheck disable=SC2086
	"$host_cc" -std=gnu11 -Wall -Wextra -Werror -rdynamic \
		$host_includes -I"$source_dir" \
		"$root/src/seat/drm_lease_seat.c" "$tmp/seat-fixture.c" \
		-L"$tmp" -Wl,--no-as-needed -lfake-seat \
		-Wl,-rpath,"$tmp" -ldl -pthread -o "$tmp/seat-fixture"
	LD_LIBRARY_PATH=$tmp "$tmp/seat-fixture" ||
		die "libseat lease validation/ownership fixture failed"
fi

/bin/sh "$cleanup_test" || die "framebuffer cleanup ownership test failed"
/bin/sh "$probe_test" || die "read-only probe test failed"

printf '%s\n' 'android DRM bridge build and contract tests: PASS'
