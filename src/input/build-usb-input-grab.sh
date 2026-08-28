#!/bin/sh
set -eu

SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SOURCE_DIR/../.." && pwd)
OUTPUT_DIR=${OUT:-$PROJECT_ROOT/build/input}
COMPILER=${CC:-cc}
EXPECTED_ARCH=${EXPECTED_ARCH:-}
OUTPUT=$OUTPUT_DIR/libandroid-usb-input-grab.so.1

mkdir -p "$OUTPUT_DIR"

"$COMPILER" \
    -std=gnu11 \
    -O2 \
    -fPIC \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -shared \
    -Wl,-z,defs \
    -Wl,-z,relro \
    -Wl,-z,now \
    -Wl,-soname,libandroid-usb-input-grab.so.1 \
    "$SOURCE_DIR/android_usb_input_grab.c" \
    -ldl \
    -o "$OUTPUT"

ln -sfn libandroid-usb-input-grab.so.1 \
    "$OUTPUT_DIR/libandroid-usb-input-grab.so"

if command -v readelf >/dev/null 2>&1; then
    readelf -h "$OUTPUT" | grep -F 'Type:' | grep -F 'DYN' >/dev/null
    case "$EXPECTED_ARCH" in
        aarch64)
            readelf -h "$OUTPUT" | grep -F 'Machine:' | grep -F 'AArch64' \
                >/dev/null
            ;;
        x86_64)
            readelf -h "$OUTPUT" | grep -F 'Machine:' | \
                grep -E 'Advanced Micro Devices X86-64|X86-64' >/dev/null
            ;;
        "") ;;
        *)
            printf '%s\n' "unsupported EXPECTED_ARCH: $EXPECTED_ARCH" >&2
            exit 2
            ;;
    esac
fi

printf '%s\n' "$OUTPUT"
