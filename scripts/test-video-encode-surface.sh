#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work_dir=${TMPDIR:-/tmp}/advc-encode-surface-test.$$
trap 'rm -rf -- "$work_dir"' EXIT HUP INT TERM
mkdir -p "$work_dir"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -I"$repo_dir/src/video_bridge/android" \
    "$repo_dir/src/video_bridge/android/encode_surface_core.c" \
    "$repo_dir/src/video_bridge/tests/encode_surface_test.c" \
    -o "$work_dir/encode_surface_test"
"$work_dir/encode_surface_test"
