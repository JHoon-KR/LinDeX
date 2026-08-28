#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${OUT:-$ROOT/build/firefox-egl-drm-identity}
CC=${CC:-cc}

mkdir -p "$OUT"
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 \
    -Wall -Wextra -Werror -fPIC -shared \
    -Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
    -Wl,-soname,liblindex-firefox-egl-drm-identity.so \
    -o "$OUT/liblindex-firefox-egl-drm-identity.so" \
    "$ROOT/src/firefox_egl_drm_identity/firefox_egl_drm_identity.c" \
    -ldl ${LDFLAGS:-}

printf '%s\n' "Firefox EGL DRM identity adapter built in $OUT"
