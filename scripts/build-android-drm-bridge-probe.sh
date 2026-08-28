#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file=$root/src/tools/android_drm_bridge_probe.c
header_dir=$root/src/bridge
out=${OUT:-$root/build/android-drm-bridge}
bridge_libdir=${BRIDGE_CORE_LIBDIR:-$out}
cc=${CC:-cc}

die() {
    printf '%s\n' "android DRM bridge probe build: $*" >&2
    exit 1
}

test -r "$source_file" || die "missing source: $source_file"
test -r "$header_dir/android_drm_bridge.h" || die "missing public bridge header"
test -r "$bridge_libdir/libandroid-drm-bridge.so.1" ||
    die "missing core library; run scripts/build-android-drm-bridge.sh first or set BRIDGE_CORE_LIBDIR"
command -v "$cc" >/dev/null 2>&1 || die "compiler not found: $cc"

mkdir -p "$out"

# Intentional word splitting permits conventional build flag variables.
# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=c99 -D_POSIX_C_SOURCE=200809L \
    -Wall -Wextra -Werror -I"$header_dir" \
    -Wl,-z,defs -Wl,-rpath,'$ORIGIN' \
    -L"$bridge_libdir" -o "$out/android-drm-bridge-probe" \
    "$source_file" ${LDFLAGS:-} -Wl,-l:libandroid-drm-bridge.so.1

test -s "$out/android-drm-bridge-probe" || die "compiler produced no probe"
printf '%s\n' "built: $out/android-drm-bridge-probe"
sha256sum "$out/android-drm-bridge-probe"
