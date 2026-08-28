#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${BRIDGE_SOURCE_DIR:-$root/src/bridge}
out=${OUT:-$root/build/android-drm-bridge}
cc=${CC:-cc}
pkg_config=${PKG_CONFIG:-pkg-config}

bridge_c=$source_dir/android_drm_bridge.c
bridge_h=$source_dir/android_drm_bridge.h
bridge_map=$source_dir/android_drm_bridge.map
blob_c=$source_dir/android_drm_blob.c
blob_h=$source_dir/android_drm_blob.h
preload_c=$source_dir/android_drm_preload.c
vulkan_policy_c=$source_dir/android_vulkan_drm_identity_policy.c
vulkan_policy_h=$source_dir/android_vulkan_drm_identity_policy.h
vulkan_preload_c=$source_dir/android_vulkan_drm_identity_preload.c
vulkan_map=$source_dir/android_vulkan_drm_identity.map
vulkan_layer_c=$source_dir/android_vulkan_drm_identity_layer.c
vulkan_layer_map=$source_dir/android_vulkan_drm_identity_layer.map

die() {
	printf '%s\n' "android DRM bridge build: $*" >&2
	exit 1
}

for source in "$bridge_c" "$bridge_h" "$bridge_map" "$blob_c" "$blob_h" \
	"$preload_c" "$vulkan_policy_c" "$vulkan_policy_h" \
	"$vulkan_preload_c" "$vulkan_map" "$vulkan_layer_c" \
	"$vulkan_layer_map"; do
	test -r "$source" || die "missing required source: $source"
done

command -v "$cc" >/dev/null 2>&1 || die "compiler not found: $cc"

drm_cflags=
drm_libs=
vulkan_cflags=
if test -n "${BRIDGE_SYSROOT:-}"; then
	sysroot=${BRIDGE_SYSROOT%/}
	include_dir=$sysroot/usr/include
	drm_include_dir=$include_dir/libdrm
	lib_dir=${BRIDGE_LIBDIR:-$sysroot/usr/lib/x86_64-linux-gnu}
	test -r "$include_dir/xf86drm.h" || die "missing sysroot header: $include_dir/xf86drm.h"
	test -r "$drm_include_dir/drm.h" || die "missing sysroot header: $drm_include_dir/drm.h"
	if test -n "${BRIDGE_VULKAN_CFLAGS:-}"; then
		vulkan_cflags=$BRIDGE_VULKAN_CFLAGS
	elif test -n "${BRIDGE_VULKAN_INCLUDE_DIR:-}"; then
		test -r "$BRIDGE_VULKAN_INCLUDE_DIR/vulkan/vulkan_core.h" ||
			die "missing Vulkan header: $BRIDGE_VULKAN_INCLUDE_DIR/vulkan/vulkan_core.h"
		vulkan_cflags="-I$BRIDGE_VULKAN_INCLUDE_DIR"
	else
		test -r "$include_dir/vulkan/vulkan_core.h" ||
			die "missing sysroot header: $include_dir/vulkan/vulkan_core.h"
		vulkan_cflags="-I$include_dir"
	fi
	test -d "$lib_dir" || die "missing sysroot library directory: $lib_dir"
	test -r "$lib_dir/libdrm.so" -o -r "$lib_dir/libdrm.a" ||
		die "missing linkable libdrm in $lib_dir"
	drm_cflags="-I$include_dir -I$drm_include_dir"
	drm_libs="-L$lib_dir -Wl,-rpath-link,$lib_dir -ldrm"
else
	command -v "$pkg_config" >/dev/null 2>&1 || die "pkg-config not found: $pkg_config"
	"$pkg_config" --exists libdrm || die "pkg-config dependency not found: libdrm"
	"$pkg_config" --exists vulkan || die "pkg-config dependency not found: vulkan"
	drm_cflags=$("$pkg_config" --cflags libdrm) || die "failed to query libdrm compiler flags"
	drm_libs=$("$pkg_config" --libs libdrm) || die "failed to query libdrm linker flags"
	vulkan_cflags=$("$pkg_config" --cflags vulkan) || die "failed to query Vulkan compiler flags"
fi

mkdir -p "$out"

# Intentional word splitting permits conventional CPPFLAGS/CFLAGS/LDFLAGS and
# pkg-config output to contain multiple arguments. Paths supplied directly to
# this script remain quoted.
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC $drm_cflags -I"$source_dir" -shared \
	-Wl,-soname,libandroid-drm-bridge.so.1 \
	-Wl,--version-script="$bridge_map" \
	-o "$out/libandroid-drm-bridge.so.1" \
	"$bridge_c" "$blob_c" ${LDFLAGS:-} $drm_libs -ldl -pthread

ln -sf libandroid-drm-bridge.so.1 "$out/libandroid-drm-bridge.so"

# The preload frontend resolves the core validation hook at runtime. Keeping
# it independently loadable ensures an absent or ambiguous core fails closed
# to the original libdrm result instead of fabricating capability data.
preload_blob_o=$out/.android_drm_blob_preload.o
# The pure helper is implementation-private in this frontend. Compile just
# that object with hidden visibility so the preload DSO exports only its two
# intentional libdrm wrappers without requiring another source-side map file.
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC -fvisibility=hidden $drm_cflags -I"$source_dir" -c \
	"$blob_c" -o "$preload_blob_o"
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC $drm_cflags -I"$source_dir" -shared \
	-Wl,-soname,libandroid-drm-preload.so \
	-o "$out/libandroid-drm-preload.so" \
	"$preload_c" "$preload_blob_o" ${LDFLAGS:-} $drm_libs -ldl -pthread
rm -f "$preload_blob_o"

# This frontend repairs only the missing Vulkan DRM identity metadata.  It is
# intentionally independent of libvulkan and resolves the loader entry points
# through LD_PRELOAD, including vkGetInstanceProcAddr dispatch.
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC -fvisibility=hidden $vulkan_cflags -I"$source_dir" -shared \
	-Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
	-Wl,-soname,libandroid-vulkan-drm-identity.so \
	-Wl,--version-script="$vulkan_map" \
	-o "$out/libandroid-vulkan-drm-identity.so" \
	"$vulkan_policy_c" "$vulkan_preload_c" ${LDFLAGS:-} -ldl -pthread

# Standard Vulkan explicit layer used by unmodified wlroots. Binding local
# entry-point calls prevents ELF interposition from recursing into libvulkan
# while the loader's global lock is held.
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC -fvisibility=hidden $vulkan_cflags -I"$source_dir" -shared \
	-Wl,-z,defs -Wl,-z,relro -Wl,-z,now -Wl,-Bsymbolic-functions \
	-Wl,-soname,libandroid-vulkan-drm-identity-layer.so \
	-Wl,--version-script="$vulkan_layer_map" \
	-o "$out/libandroid-vulkan-drm-identity-layer.so" \
	"$vulkan_policy_c" "$vulkan_layer_c" ${LDFLAGS:-} -ldl -pthread

for artifact in \
	"$out/libandroid-drm-bridge.so.1" \
	"$out/libandroid-drm-preload.so" \
	"$out/libandroid-vulkan-drm-identity.so" \
	"$out/libandroid-vulkan-drm-identity-layer.so"; do
	test -s "$artifact" || die "compiler did not produce a non-empty artifact: $artifact"
done

printf '%s\n' \
	"built: $out/libandroid-drm-bridge.so.1" \
	"built: $out/libandroid-drm-preload.so" \
	"built: $out/libandroid-vulkan-drm-identity.so" \
	"built: $out/libandroid-vulkan-drm-identity-layer.so"
