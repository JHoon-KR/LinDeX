#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_script=$root/scripts/build-android-drm-bridge-probe.sh
probe_source=$root/src/tools/android_drm_bridge_probe.c
header_dir=$root/src/bridge
cc=${CC:-cc}

die() {
	printf '%s\n' "android DRM bridge probe test: $*" >&2
	exit 1
}

require_text() {
	pattern=$1
	file=$2
	grep -Fq -- "$pattern" "$file" ||
		die "missing invariant in $file: $pattern"
}

for file in "$build_script" "$probe_source" \
	"$header_dir/android_drm_bridge.h"; do
	test -r "$file" || die "missing input: $file"
done
/bin/sh -n "$build_script"
/bin/sh -n "$0"
command -v "$cc" >/dev/null 2>&1 || die "compiler not found: $cc"

require_text 'DRM_LEASE_FD' "$probe_source"
require_text 'DRM_LEASE_OBJECTS' "$probe_source"
require_text 'adbr_lease_dup_validate_v1' "$probe_source"
require_text 'adbr_plane_query_formats_v1' "$probe_source"
require_text 'adbr_preload_validate_plane_blob_v1' "$probe_source"
require_text 'vendor-candidate-not-active-proof' "$probe_source"

# The diagnostic must remain observation-only. This source-level contract is
# intentionally narrower than auditing the bridge core that it links.
if grep -Eiq 'adbr_fb_|AddFB|atomic|modeset|commit' "$probe_source"; then
	die "probe source contains a framebuffer or display mutation operation"
fi

tmp=${TMPDIR:-/tmp}/android-drm-bridge-probe-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/lib" "$tmp/out" "$tmp/missing"

cat >"$tmp/fake-core.c" <<'EOF'
#include "android_drm_bridge.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define XB24 UINT32_C(0x34324258)

uint32_t adbr_get_abi_version(void)
{
	return ADBR_ABI_VERSION_1;
}

uint64_t adbr_get_capabilities(void)
{
	return ADBR_CAP_LEASE_VALIDATE | ADBR_CAP_PLANE_FORMATS |
	       ADBR_CAP_QCOM_CANDIDATES | ADBR_CAP_PRELOAD_VALIDATE;
}

int adbr_lease_dup_validate_v1(int fd, const uint32_t *objects, size_t count,
			       uint32_t flags, int *out_fd,
			       struct adbr_lease_info_v1 *info)
{
	static const uint32_t exact[] = { 11, 22, 33 };
	size_t i;
	(void)flags;
	if (fd < 0 || !objects || count != 3 || !out_fd || !info)
		return -EINVAL;
	for (i = 0; i < count; ++i) {
		if (objects[i] != exact[i])
			return -EPERM;
	}
	*out_fd = dup(fd);
	if (*out_fd < 0)
		return -errno;
	info->object_count = 3;
	return 0;
}

int adbr_lease_get_objects_v1(int fd, uint32_t *objects, size_t *count)
{
	(void)fd;
	if (!count)
		return -EINVAL;
	if (!objects) {
		*count = 3;
		return 0;
	}
	if (*count < 3)
		return -ENOSPC;
	objects[0] = 11;
	objects[1] = 22;
	objects[2] = 33;
	*count = 3;
	return 0;
}

int adbr_plane_query_formats_v1(int fd, uint32_t plane_id,
				uint32_t flags,
				struct adbr_format_modifier_v1 *entries,
				size_t *count)
{
	(void)fd;
	if (plane_id != 22 || flags != ADBR_PLANE_QUERY_QCOM_CANDIDATES ||
	    !count)
		return -EINVAL;
	if (!entries) {
		*count = 2;
		return 0;
	}
	if (*count < 2)
		return -ENOSPC;
	entries[0].fourcc = XB24;
	entries[0].modifier = ADBR_MODIFIER_LINEAR;
	entries[0].source_flags = ADBR_FORMAT_SOURCE_IN_FORMATS;
	entries[1].fourcc = XB24;
	entries[1].modifier = ADBR_MODIFIER_QCOM_COMPRESSED;
	entries[1].source_flags = ADBR_FORMAT_SOURCE_QCOM_CANDIDATE;
	*count = 2;
	return 0;
}

int adbr_preload_validate_plane_blob_v1(int fd, uint32_t plane_id,
					uint32_t blob_id, uint32_t fourcc,
					uint64_t modifier)
{
	(void)fd;
	return plane_id == 22 && blob_id == 77 && fourcc == XB24 &&
	       modifier == ADBR_MODIFIER_QCOM_COMPRESSED ? 0 : -EPERM;
}
EOF

# Build a bounded fake implementation of the public ABI, then use the real
# probe build script. This exercises linking and every successful CLI mode
# without pretending the host owns an Android DRM lease.
"$cc" -std=c99 -Wall -Wextra -Werror -fPIC -shared \
	-Wl,-soname,libandroid-drm-bridge.so.1 -I"$header_dir" \
	-o "$tmp/lib/libandroid-drm-bridge.so.1" "$tmp/fake-core.c"
OUT=$tmp/out BRIDGE_CORE_LIBDIR=$tmp/lib CC=$cc "$build_script" >/dev/null
probe=$tmp/out/android-drm-bridge-probe
test -x "$probe" || die "build did not produce an executable probe"
cp "$tmp/lib/libandroid-drm-bridge.so.1" "$tmp/out/"

run_probe() {
	output=$1
	shift
	(
		exec 9</dev/null
		DRM_LEASE_FD=9 DRM_LEASE_OBJECTS=11,22,33 "$probe" "$@"
	) >"$output"
}

run_probe "$tmp/lease.out" --lease
require_text 'ADBR-PROBE schema=1 operation=lease result=PASS' "$tmp/lease.out"
require_text 'ADBR-OBJECT index=2 id=33' "$tmp/lease.out"

run_probe "$tmp/plane.out" --plane 22
require_text 'ADBR-PROBE schema=1 operation=plane result=PASS' "$tmp/plane.out"
require_text 'pairs=2' "$tmp/plane.out"
require_text 'modifier=0x0000000000000000 source=in-formats' "$tmp/plane.out"
require_text 'modifier=0x0500000000000001 source=qcom-candidate' "$tmp/plane.out"

run_probe "$tmp/preload.out" --preload-gate 22 77
require_text 'ADBR-PROBE schema=1 operation=preload-gate result=PASS' \
	"$tmp/preload.out"
require_text 'authority=vendor-candidate-not-active-proof' "$tmp/preload.out"

expect_reject() {
	name=$1
	shift
	if "$@" >"$tmp/$name.out" 2>&1; then
		die "probe accepted hostile input: $name"
	fi
	require_text 'result=FAIL' "$tmp/$name.out"
	test "$(wc -c <"$tmp/$name.out")" -le 256 ||
		die "failure result is not bounded: $name"
}

expect_reject missing-environment env -u DRM_LEASE_FD -u DRM_LEASE_OBJECTS \
	"$probe" --lease
expect_reject signed-fd env DRM_LEASE_FD=+9 DRM_LEASE_OBJECTS=11,22,33 \
	"$probe" --lease
expect_reject duplicate-object env DRM_LEASE_FD=9 DRM_LEASE_OBJECTS=11,22,22 \
	"$probe" --lease
expect_reject whitespace-object env DRM_LEASE_FD=9 'DRM_LEASE_OBJECTS=11, 22,33' \
	"$probe" --lease
expect_reject trailing-comma env DRM_LEASE_FD=9 DRM_LEASE_OBJECTS=11,22,33, \
	"$probe" --lease
expect_reject plane-outside-manifest env DRM_LEASE_FD=9 \
	DRM_LEASE_OBJECTS=11,22,33 "$probe" --plane 44
expect_reject zero-plane env DRM_LEASE_FD=9 DRM_LEASE_OBJECTS=11,22,33 \
	"$probe" --plane 0
expect_reject overflow-blob env DRM_LEASE_FD=9 DRM_LEASE_OBJECTS=11,22,33 \
	"$probe" --preload-gate 22 4294967296

if OUT=$tmp/reject BRIDGE_CORE_LIBDIR=$tmp/missing CC=$cc \
	"$build_script" >/dev/null 2>&1; then
	die "probe build accepted a missing bridge core"
fi

printf '%s\n' 'android DRM bridge probe host tests: PASS'
