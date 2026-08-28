#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/firefox-advc-rdd-socket}
CC=${CC:-cc}

mkdir -p "$OUT"
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 \
    -Wall -Wextra -Werror -fPIC -shared \
    -Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
    -Wl,-soname,liblindex-firefox-advc-rdd-socket.so \
    -o "$OUT/liblindex-firefox-advc-rdd-socket.so" \
    "$ROOT/src/firefox_advc_rdd_socket/firefox_advc_rdd_socket.c" \
    -ldl ${LDFLAGS:-}

printf '%s\n' "Firefox RDD ADVC socket adapter built in $OUT"
