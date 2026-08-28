#!/bin/sh
# Build the LinDeX v3 bridge without compositor-specific patches or adapters.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/bridge-runtime}
CC=${CC:-cc}
EXPECTED_ARCH=${EXPECTED_ARCH:-}

mkdir -p "$OUT"

OUT="$OUT" CC="$CC" sh "$ROOT/scripts/build-android-drm-bridge.sh"
OUT="$OUT" BRIDGE_CORE_LIBDIR="$OUT" CC="$CC" \
    sh "$ROOT/scripts/build-android-drm-bridge-probe.sh"

"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 \
    -Wall -Wextra -Werror -fPIC -shared \
    -Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
    -Wl,-soname,libdrm_lease_seat.so -Wl,-rpath,'$ORIGIN' \
    -I"$ROOT/src/bridge" -L"$OUT" \
    -o "$OUT/libdrm_lease_seat.so" "$ROOT/src/seat/drm_lease_seat.c" \
    -Wl,-l:libandroid-drm-bridge.so.1 -ldl -pthread ${LDFLAGS:-}

OUT="$OUT" CC="$CC" EXPECTED_ARCH="$EXPECTED_ARCH" \
    sh "$ROOT/src/input/build-usb-input-grab.sh"

for artifact in \
    libandroid-drm-bridge.so.1 \
    libandroid-drm-preload.so \
    libandroid-vulkan-drm-identity.so \
    libandroid-vulkan-drm-identity-layer.so \
    android-drm-bridge-probe \
    libdrm_lease_seat.so \
    libandroid-usb-input-grab.so.1
do
    test -s "$OUT/$artifact" || {
        printf '%s\n' "missing build artifact: $OUT/$artifact" >&2
        exit 1
    }
done

mkdir -p "$OUT/share/vulkan/explicit_layer.d"
install -m 0644 "$ROOT/src/bridge/VK_LAYER_LINDEX_android_drm_identity.json" \
    "$OUT/share/vulkan/explicit_layer.d/VK_LAYER_LINDEX_android_drm_identity.json"

printf '%s\n' "LinDeX bridge runtime built in $OUT"
