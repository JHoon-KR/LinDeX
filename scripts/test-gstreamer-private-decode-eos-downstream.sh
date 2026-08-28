#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
patch_file=$repo_dir/patches/gstreamer/0001-vaapi-add-prerequisite-external-surface-release-hook.patch
hash_file=$repo_dir/patches/gstreamer/gstreamer-vaapi-1.26.2-source.sha256
model_test=$repo_dir/tests/gstreamer/gstvaapi_advc_eos_prerequisite_model_test.c
source_dir=${GSTREAMER_VAAPI_SOURCE_DIR:-}
full_build=no

fail() {
    printf 'gstreamer private decode EOS prerequisite test failed: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<EOF
Usage: $0 [--full-build] SOURCE_DIR

SOURCE_DIR must be an unmodified gstreamer-vaapi 1.26.2 upstream source tree.
--full-build additionally runs a strict Meson/Ninja build of the patched tree.
GSTREAMER_VAAPI_SOURCE_DIR may supply SOURCE_DIR.
EOF
}

if [ "${1:-}" = "--full-build" ]; then
    full_build=yes
    shift
fi
if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    source_dir=$1
fi
[ -n "$source_dir" ] || {
    usage >&2
    exit 2
}
[ -d "$source_dir" ] || fail "source directory does not exist: $source_dir"

for tool in git sha256sum awk sed grep mktemp cc; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
done
if [ "$full_build" = yes ]; then
    for tool in meson ninja; do
        command -v "$tool" >/dev/null 2>&1 || fail "required build tool not found: $tool"
    done
fi

sh -n "$0"
[ -f "$patch_file" ] || fail "missing patch: $patch_file"
[ -f "$hash_file" ] || fail "missing source hash manifest: $hash_file"
[ -f "$model_test" ] || fail "missing unit model: $model_test"

grep -Fq "version : '1.26.2'" "$source_dir/meson.build" ||
    fail 'source version is not exactly 1.26.2'

while read -r expected relative; do
    case "$expected" in
        ''|'#'*) continue ;;
    esac
    [ -f "$source_dir/$relative" ] || fail "missing source anchor: $relative"
    actual=$(sha256sum "$source_dir/$relative" | awk '{print $1}')
    [ "$actual" = "$expected" ] ||
        fail "source drift for $relative: expected $expected, got $actual"
done <"$hash_file"

(cd "$source_dir" && git -c core.fileMode=false apply --check "$patch_file") ||
    fail 'patch does not apply cleanly to the exact source tree'

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lindex-gst-eos-hooks.XXXXXX")
cleanup() {
    rm -rf -- "$work_dir"
}
trap cleanup EXIT HUP INT TERM

if [ "$full_build" = yes ]; then
    mkdir -p "$work_dir/source"
    cp -R "$source_dir/." "$work_dir/source/"
else
    mkdir -p "$work_dir/source/gst-libs/gst/vaapi" \
        "$work_dir/source/gst/vaapi"
    cp "$source_dir/meson.build" "$work_dir/source/meson.build"
    for relative in \
        gst-libs/gst/vaapi/gstvaapidecoder.c \
        gst-libs/gst/vaapi/gstvaapidecoder.h \
        gst-libs/gst/vaapi/gstvaapisurfaceproxy.c \
        gst-libs/gst/vaapi/gstvaapisurfaceproxy.h \
        gst-libs/gst/vaapi/gstvaapisurfaceproxy_priv.h \
        gst/vaapi/gstvaapidecode.c; do
        cp "$source_dir/$relative" "$work_dir/source/$relative"
    done
fi
(cd "$work_dir/source" && git -c core.fileMode=false apply "$patch_file")

proxy_c=$work_dir/source/gst-libs/gst/vaapi/gstvaapisurfaceproxy.c
release_line=$(grep -n 'proxy->release_func (proxy->release_data)' "$proxy_c" |
    sed -n '1s/:.*//p')
pool_line=$(grep -n 'gst_vaapi_video_pool_put_object' "$proxy_c" |
    sed -n '1s/:.*//p')
[ -n "$release_line" ] && [ -n "$pool_line" ] ||
    fail 'patched release/pool ordering anchors are missing'
[ "$release_line" -lt "$pool_line" ] ||
    fail 'release callback does not precede pool availability'

grep -Fq 'if (can_recycle)' "$proxy_c" ||
    fail 'release failure does not quarantine the surface'
grep -Fq 'gst_vaapi_decoder_get_va_display' \
    "$work_dir/source/gst-libs/gst/vaapi/gstvaapidecoder.c" ||
    fail 'VADisplay accessor is missing'
grep -Fq 'gst_vaapi_decoder_get_va_context' \
    "$work_dir/source/gst-libs/gst/vaapi/gstvaapidecoder.c" ||
    fail 'VAContextID accessor is missing'

if grep -Eq 'advcVaGetDecodeEosInterface|vaGetLibFunc|gst_vaapidecode_finish' \
    "$patch_file"; then
    fail 'prerequisite patch must not wire the unprovable EOS finish path'
fi
actual_finish_hash=$(sha256sum "$work_dir/source/gst/vaapi/gstvaapidecode.c" |
    awk '{print $1}')
[ "$actual_finish_hash" = \
    '06a1f9e71079ed69353bd4f8fa47990feee503e43fbe5c3061a9ab8ca9b41835' ] ||
    fail 'decoder finish source changed unexpectedly'

cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
    "$model_test" -o "$work_dir/gstvaapi-eos-prerequisite-model"
"$work_dir/gstvaapi-eos-prerequisite-model"

if [ "$full_build" = yes ]; then
    meson setup --werror "$work_dir/build" "$work_dir/source"
    ninja -C "$work_dir/build"
fi

printf 'gstreamer-vaapi 1.26.2 drift/apply/static checks: PASS\n'
if [ "$full_build" = yes ]; then
    printf 'gstreamer-vaapi 1.26.2 strict full build: PASS\n'
else
    printf 'full plugin build: NOT RUN (pass --full-build with development dependencies)\n'
fi
