#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=$(mktemp -d)
BINARY=$BUILD_DIR/drm_lease_broker_auto_fixture
cleanup()
{
    rm -f -- "$BINARY"
    rmdir -- "$BUILD_DIR" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

${CC:-cc} -std=gnu11 -O2 -Wall -Wextra \
    -Werror -Wno-unused-function -Wno-unused-result \
    -o "$BINARY" \
    "$ROOT/tests/broker/drm_lease_broker_auto_fixture.c"
"$BINARY"
