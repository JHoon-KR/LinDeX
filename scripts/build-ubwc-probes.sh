#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build/ubwc-probes"}
cc=${CC:-cc}

mkdir -p "$out"

"$cc" -O2 -Wall -Wextra \
    "$root/src/tools/vulkan_modifier_probe.c" \
    -o "$out/vulkan_modifier_probe" \
    -lvulkan

"$cc" -O2 -Wall -Wextra \
    "$root/src/tools/vulkan_ubwc_addfb2_probe.c" \
    -o "$out/vulkan_ubwc_addfb2_probe" \
    $(pkg-config --cflags --libs vulkan libdrm)

protocols=${WAYLAND_PROTOCOLS_DIR:-/usr/share/wayland-protocols}
scanner=${WAYLAND_SCANNER:-wayland-scanner}
"$scanner" client-header \
    "$protocols/stable/xdg-shell/xdg-shell.xml" \
    "$out/xdg-shell-client-protocol.h"
"$scanner" private-code \
    "$protocols/stable/xdg-shell/xdg-shell.xml" \
    "$out/xdg-shell-protocol.c"

"$cc" -O2 -Wall -Wextra -I"$out" \
    "$root/src/tools/vulkan_wayland_ab24.c" \
    "$out/xdg-shell-protocol.c" \
    -o "$out/vulkan_wayland_ab24" \
    $(pkg-config --cflags --libs wayland-client vulkan)

printf '%s\n' "built:" \
    "$out/vulkan_modifier_probe" \
    "$out/vulkan_ubwc_addfb2_probe" \
    "$out/vulkan_wayland_ab24"
