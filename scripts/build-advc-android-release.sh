#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SOURCE=$ROOT/src/video_bridge
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to Android NDK r27d or newer}"
OUT=${OUT:-$ROOT/build/advc-android-release}
BUILD_PARENT=${TMPDIR:-/tmp}
BUILD_ROOT=$(mktemp -d "$BUILD_PARENT/lindex-advc-release.XXXXXX")

case "$BUILD_ROOT" in
    "$BUILD_PARENT"/lindex-advc-release.*) ;;
    *) printf 'Unsafe temporary build root: %s\n' "$BUILD_ROOT" >&2; exit 1 ;;
esac
trap 'rm -rf -- "$BUILD_ROOT"' EXIT HUP INT TERM

build_one() {
    build_dir=$1
    SOURCE_DATE_EPOCH=946684800 cmake -S "$SOURCE" -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-28 \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--build-id=none \
        -DCMAKE_BUILD_TYPE=Release
    SOURCE_DATE_EPOCH=946684800 cmake --build "$build_dir" \
        --target advc-broker advc-capability-probe -j8
    "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
        "$build_dir/advc-broker" "$build_dir/advc-capability-probe"
}

build_one "$BUILD_ROOT/a"
build_one "$BUILD_ROOT/b"
cmp "$BUILD_ROOT/a/advc-broker" "$BUILD_ROOT/b/advc-broker"
cmp "$BUILD_ROOT/a/advc-capability-probe" \
    "$BUILD_ROOT/b/advc-capability-probe"

mkdir -p "$OUT"
install -m 0755 "$BUILD_ROOT/a/advc-broker" "$OUT/advc-broker"
install -m 0755 "$BUILD_ROOT/a/advc-capability-probe" \
    "$OUT/advc-capability-probe"

file "$OUT/advc-broker" "$OUT/advc-capability-probe"
sha256sum "$OUT/advc-broker" "$OUT/advc-capability-probe"
