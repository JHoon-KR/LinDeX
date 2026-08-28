#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE=$ROOT/src/bridge
TEST=$ROOT/tests/bridge/vulkan_drm_identity_test.c
MAP=$SOURCE/android_vulkan_drm_identity.map
LAYER_MAP=$SOURCE/android_vulkan_drm_identity_layer.map
LAYER_ACK_TEST=$ROOT/tests/bridge/vulkan_drm_identity_layer_ack_test.c
LAYER_LIFECYCLE_TEST=$ROOT/tests/bridge/vulkan_drm_identity_layer_lifecycle_test.c
TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT HUP INT TERM
CC=${CC:-cc}
PKG_CONFIG=${PKG_CONFIG:-pkg-config}

fail() {
	printf '%s\n' "Vulkan DRM identity test failed: $*" >&2
	exit 1
}

for file in \
	android_vulkan_drm_identity_policy.h \
	android_vulkan_drm_identity_policy.c \
	android_vulkan_drm_identity_preload.c \
	android_vulkan_drm_identity_test.h \
	android_vulkan_drm_identity.map \
	android_vulkan_drm_identity_layer.c \
	android_vulkan_drm_identity_layer.map; do
	test -r "$SOURCE/$file" || fail "missing source: $file"
done
test -r "$TEST" || fail "missing fake-callback fixture"
test -r "$LAYER_ACK_TEST" || fail "missing layer ACK fixture"
test -r "$LAYER_LIFECYCLE_TEST" || fail "missing layer lifecycle fixture"
command -v "$CC" >/dev/null 2>&1 || fail "compiler not found: $CC"
command -v "$PKG_CONFIG" >/dev/null 2>&1 || fail "pkg-config not found"
"$PKG_CONFIG" --exists vulkan || fail "vulkan headers not found"
vulkan_cflags=$("$PKG_CONFIG" --cflags vulkan)

# shellcheck disable=SC2086
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-DADVK_TESTING $vulkan_cflags -I"$SOURCE" \
	-o "$TMP/vulkan-drm-identity-test" \
	"$SOURCE/android_vulkan_drm_identity_policy.c" \
	"$SOURCE/android_vulkan_drm_identity_preload.c" "$TEST" \
	${LDFLAGS:-} -ldl -pthread
"$TMP/vulkan-drm-identity-test"

# Exercise the v2 runtime rdev ACK parser independently of a Vulkan loader.
# shellcheck disable=SC2086
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-DADVK_LAYER_TESTING $vulkan_cflags -I"$SOURCE" \
	-o "$TMP/vulkan-drm-identity-layer-ack-test" \
	"$SOURCE/android_vulkan_drm_identity_policy.c" \
	"$SOURCE/android_vulkan_drm_identity_layer.c" "$LAYER_ACK_TEST" \
	${LDFLAGS:-} -ldl -pthread
"$TMP/vulkan-drm-identity-layer-ack-test"

# Bound the dispatch tables to one slot so exhaustion, downstream cleanup,
# slot reuse, and physical-device-group ownership are all deterministic.
# shellcheck disable=SC2086
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-DADVK_LAYER_TESTING -DMAX_INSTANCES=1 -DMAX_DEVICES=1 \
	-DMAX_PHYSICAL_DEVICES=4 $vulkan_cflags -I"$SOURCE" \
	-o "$TMP/vulkan-drm-identity-layer-lifecycle-test" \
	"$SOURCE/android_vulkan_drm_identity_policy.c" \
	"$SOURCE/android_vulkan_drm_identity_layer.c" "$LAYER_LIFECYCLE_TEST" \
	${LDFLAGS:-} -ldl -pthread
"$TMP/vulkan-drm-identity-layer-lifecycle-test"

# Build the production DSO and enforce its four-symbol, versioned ABI.
# shellcheck disable=SC2086
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC -fvisibility=hidden $vulkan_cflags -I"$SOURCE" -shared \
	-Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
	-Wl,-soname,libandroid-vulkan-drm-identity.so \
	-Wl,--version-script="$MAP" \
	-o "$TMP/libandroid-vulkan-drm-identity.so" \
	"$SOURCE/android_vulkan_drm_identity_policy.c" \
	"$SOURCE/android_vulkan_drm_identity_preload.c" \
	${LDFLAGS:-} -ldl -pthread

if command -v nm >/dev/null 2>&1; then
	nm -D --defined-only "$TMP/libandroid-vulkan-drm-identity.so" |
		awk '{ print $3 }' | sed 's/@@.*//' | sort -u >"$TMP/symbols"
	cat >"$TMP/expected" <<'EOF'
ANDROID_VULKAN_DRM_IDENTITY_1.0
vkEnumerateDeviceExtensionProperties
vkGetInstanceProcAddr
vkGetPhysicalDeviceProperties2
vkGetPhysicalDeviceProperties2KHR
EOF
	diff -u "$TMP/expected" "$TMP/symbols" || fail "unexpected exported ABI"
fi
if command -v readelf >/dev/null 2>&1; then
	readelf --version-info "$TMP/libandroid-vulkan-drm-identity.so" |
		grep -Fq 'ANDROID_VULKAN_DRM_IDENTITY_1.0' ||
		fail "version definition is missing"
fi

printf '%s\n' 'Vulkan DRM identity symbol/version tests: PASS'

# Build the standard explicit layer. Binding its own Vulkan entry points is
# mandatory: otherwise ELF preemption can recurse into the loader while its
# global mutex is held.
# shellcheck disable=SC2086
"$CC" ${CPPFLAGS:-} ${CFLAGS:--O2} -std=gnu11 -Wall -Wextra -Werror \
	-fPIC -fvisibility=hidden $vulkan_cflags -I"$SOURCE" -shared \
	-Wl,-z,defs -Wl,-z,relro -Wl,-z,now -Wl,-Bsymbolic-functions \
	-Wl,-soname,libandroid-vulkan-drm-identity-layer.so \
	-Wl,--version-script="$LAYER_MAP" \
	-o "$TMP/libandroid-vulkan-drm-identity-layer.so" \
	"$SOURCE/android_vulkan_drm_identity_policy.c" \
	"$SOURCE/android_vulkan_drm_identity_layer.c" \
	${LDFLAGS:-} -ldl -pthread

if command -v nm >/dev/null 2>&1; then
	nm -D --defined-only "$TMP/libandroid-vulkan-drm-identity-layer.so" |
		awk '{ print $3 }' | sed 's/@@.*//' | sort -u >"$TMP/layer-symbols"
	cat >"$TMP/layer-expected" <<'EOF'
LINDEX_VULKAN_DRM_IDENTITY_LAYER_1.0
lindexGetPhysicalDeviceProcAddr
vkCreateDevice
vkCreateInstance
vkDestroyDevice
vkDestroyInstance
vkEnumerateDeviceExtensionProperties
vkEnumerateInstanceExtensionProperties
vkEnumerateInstanceLayerProperties
vkEnumeratePhysicalDeviceGroups
vkEnumeratePhysicalDeviceGroupsKHR
vkEnumeratePhysicalDevices
vkGetDeviceProcAddr
vkGetInstanceProcAddr
vkGetPhysicalDeviceProperties2
vkGetPhysicalDeviceProperties2KHR
vkNegotiateLoaderLayerInterfaceVersion
EOF
	diff -u "$TMP/layer-expected" "$TMP/layer-symbols" ||
		fail "unexpected explicit-layer ABI"
fi

printf '%s\n' 'Vulkan DRM identity explicit-layer tests: PASS'
