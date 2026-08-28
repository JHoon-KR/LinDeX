#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/aarch64}
ZIG=${ZIG:-zig}
mkdir -p "$OUT"

for source in \
    src/broker/drm_lease_broker.c \
    src/broker/drm_lease_client.c \
    src/tools/drm_feature_probe.c
do
    name=$(basename "$source" .c)
    "$ZIG" cc -target aarch64-linux-musl -std=gnu11 -O2 -static -s \
        -Wall -Wextra -o "$OUT/$name" "$ROOT/$source"
    file "$OUT/$name"
    sha256sum "$OUT/$name"
done
