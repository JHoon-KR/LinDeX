#!/bin/sh
# Reproducibly cross-build the ARM64 ADVC libva vendor driver. The output is a
# release runtime component, not a smoke/probe tool.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE=$REPO/src/vaapi_advc
OUTPUT=${1:-$REPO/module/payload/debian/codec/advc_drv_video.so}
PREFLIGHT_OUTPUT=${ADVC_VAAPI_PREFLIGHT_OUTPUT:-${OUTPUT%/*}/advc-vaapi-decode-preflight}
BUILD_ROOT=${ADVC_VAAPI_BUILD_ROOT:-/tmp/lindex-advc-vaapi-release}
CC=${ADVC_AARCH64_CC:-aarch64-linux-gnu-gcc}
STRIP=${ADVC_AARCH64_STRIP:-aarch64-linux-gnu-strip}
LIBVA_INCLUDE_DIR=${LIBVA_INCLUDE_DIR:-}

command -v cmake >/dev/null 2>&1 || { echo 'cmake is required' >&2; exit 69; }
command -v ninja >/dev/null 2>&1 || { echo 'ninja is required' >&2; exit 69; }
command -v "$CC" >/dev/null 2>&1 || { echo "$CC is required" >&2; exit 69; }
command -v "$STRIP" >/dev/null 2>&1 || { echo "$STRIP is required" >&2; exit 69; }

if [ -z "$LIBVA_INCLUDE_DIR" ]; then
    for candidate in /usr/include /usr/local/include; do
        if [ -f "$candidate/va/va_backend.h" ]; then
            LIBVA_INCLUDE_DIR=$candidate
            break
        fi
    done
fi
[ -f "$LIBVA_INCLUDE_DIR/va/va_backend.h" ] || {
    echo 'set LIBVA_INCLUDE_DIR to a libva development include directory' >&2
    exit 69
}

# The build directory is recursively replaced, so confine the only destructive
# operation to an explicit task-owned path under /tmp.
case "$BUILD_ROOT" in
    /tmp/lindex-advc-vaapi-*) ;;
    *) echo 'ADVC_VAAPI_BUILD_ROOT must be /tmp/lindex-advc-vaapi-*' >&2; exit 64 ;;
esac
rm -rf "$BUILD_ROOT"
cmake -S "$SOURCE" -B "$BUILD_ROOT" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_INCLUDE_PATH="$LIBVA_INCLUDE_DIR" \
    -DADVC_VAAPI_ENABLE_INPROCESS_REPACK=OFF \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_ROOT" --target advc_drv_video \
    advc-vaapi-decode-preflight --parallel
"$STRIP" --strip-unneeded "$BUILD_ROOT/advc_drv_video.so"
"$STRIP" --strip-unneeded "$BUILD_ROOT/advc-vaapi-decode-preflight"

mkdir -p "${OUTPUT%/*}" "${PREFLIGHT_OUTPUT%/*}"
cp "$BUILD_ROOT/advc_drv_video.so" "$OUTPUT"
cp "$BUILD_ROOT/advc-vaapi-decode-preflight" "$PREFLIGHT_OUTPUT"
chmod 0755 "$OUTPUT"
chmod 0755 "$PREFLIGHT_OUTPUT"
sha256sum "$OUTPUT" "$PREFLIGHT_OUTPUT"
