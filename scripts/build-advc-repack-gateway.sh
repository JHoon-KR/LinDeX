#!/bin/sh
# Cross-build the ARM64 LinDeX ADVC repack gateway for the Debian chroot.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE=$REPO/src/video_bridge
OUTPUT=${1:-$REPO/module/payload/debian/codec/advc-repack-gateway}
BUILD_ROOT=${ADVC_GATEWAY_BUILD_ROOT:-/tmp/lindex-advc-repack-gateway-release}
CC=${ADVC_AARCH64_CC:-aarch64-linux-gnu-gcc}
STRIP=${ADVC_AARCH64_STRIP:-aarch64-linux-gnu-strip}
VULKAN_INCLUDE_DIR=${VULKAN_INCLUDE_DIR:-/usr/include}
VULKAN_LIBRARY=${VULKAN_LIBRARY:-/usr/lib/aarch64-linux-gnu/libvulkan.so}

case "$BUILD_ROOT" in
    /tmp/lindex-advc-repack-gateway-*) ;;
    *) echo 'ADVC_GATEWAY_BUILD_ROOT must be /tmp/lindex-advc-repack-gateway-*' >&2; exit 64 ;;
esac
for command_name in cmake ninja "$CC" "$STRIP"; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "$command_name is required" >&2
        exit 69
    }
done
[ -f "$VULKAN_INCLUDE_DIR/vulkan/vulkan.h" ] || {
    echo 'Vulkan headers are required' >&2
    exit 69
}
[ -f "$VULKAN_LIBRARY" ] || {
    echo 'AArch64 libvulkan linker input is required' >&2
    exit 69
}

rm -rf "$BUILD_ROOT"
cmake -S "$SOURCE" -B "$BUILD_ROOT" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$CC" \
    -DVulkan_INCLUDE_DIR="$VULKAN_INCLUDE_DIR" \
    -DVulkan_LIBRARY="$VULKAN_LIBRARY" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_ROOT" --target advc-repack-gateway --parallel
"$STRIP" --strip-unneeded "$BUILD_ROOT/advc-repack-gateway"
mkdir -p "${OUTPUT%/*}"
cp "$BUILD_ROOT/advc-repack-gateway" "$OUTPUT"
chmod 0755 "$OUTPUT"
sha256sum "$OUTPUT"
