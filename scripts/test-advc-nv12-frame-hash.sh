#!/bin/sh
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf -- "$TMPDIR_TEST"' EXIT HUP INT TERM

CC=${CC:-cc}
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
    "$REPO/src/vaapi_advc/tests/advc_nv12_frame_hash.c" \
    -o "$TMPDIR_TEST/advc-nv12-frame-hash"

printf '\000\001\002\003\004\005\006\007\010\011\012\013' > \
    "$TMPDIR_TEST/two-frames.nv12"
actual=$($TMPDIR_TEST/advc-nv12-frame-hash \
    "$TMPDIR_TEST/two-frames.nv12" 2 2)
expected='poc=0 fnv1a64=8c236b41ee28a2b4 bytes=6
poc=1 fnv1a64=0ff69455ad14f648 bytes=6'
[ "$actual" = "$expected" ] || {
    printf '%s\n' "unexpected NV12 fixture hash:" "$actual" >&2
    exit 1
}

printf '\000' > "$TMPDIR_TEST/partial.nv12"
if "$TMPDIR_TEST/advc-nv12-frame-hash" \
    "$TMPDIR_TEST/partial.nv12" 2 2 >/dev/null 2>&1; then
    echo 'partial-frame fixture unexpectedly succeeded' >&2
    exit 1
fi

echo 'ADVC NV12 frame hash fixture: PASS'
