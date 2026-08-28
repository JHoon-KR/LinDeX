#!/usr/bin/env bash
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
CONTROL=$REPO/module/bin/debian-gpu-control
COMMON=$REPO/module/bin/common.sh
LAUNCH=$REPO/module/bin/launch-stock-profile
BOOT=$REPO/module/bin/bootdebian
SETUP=$REPO/module/bin/setup-debian-gpu
SESSION=$REPO/module/bin/stock-profile-session
KGSL_PROFILE=$REPO/module/payload/debian/99-android-kgsl.sh
PROFILE_CLIENT=$REPO/module/payload/debian/start-profile-client
APP=$REPO/module/webroot/app.js
INDEX=$REPO/module/webroot/index.html
LOCALES=$REPO/module/webroot/locales.js
BRIDGE=$REPO/module/payload/bridge

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
require_text() {
    grep -Fq -- "$1" "$2" || fail "missing '$1' in ${2#$REPO/}"
}
require_after() {
    local earlier=$1 later=$2 file=$3 earlier_line later_line
    earlier_line=$(grep -nF -- "$earlier" "$file" | head -n1 | cut -d: -f1)
    later_line=$(grep -nF -- "$later" "$file" | head -n1 | cut -d: -f1)
    [ -n "$earlier_line" ] && [ -n "$later_line" ] &&
        [ "$later_line" -gt "$earlier_line" ] ||
        fail "'$later' must follow '$earlier' in ${file#$REPO/}"
}

bash -n "$CONTROL" "$COMMON" "$LAUNCH" "$BOOT" "$SETUP" "$SESSION" \
    "$REPO/module/payload/debian/android-drm-install" \
    "$REPO/module/payload/debian/android-drm-profile-manager" \
    "$REPO/module/payload/debian/android-drm-provider-manager"

require_after 'mount -o bind /dev "$ROOTFS/dev"' \
    'mkdir -p "$ROOTFS/dev/pts" "$ROOTFS/dev/shm"' "$COMMON"
require_text '[ -e "$ROOTFS/proc/self/fd" ]' "$COMMON"
require_text '[ -d "$ROOTFS/sys/class/drm" ]' "$COMMON"
for mount_script in "$BOOT" "$LAUNCH" "$SETUP"; do
    require_text 'ensure_chroot_mounts' "$mount_script"
done

(cd "$BRIDGE" && sha256sum -c BRIDGE_PAYLOAD.sha256 >/dev/null)
(cd "$BRIDGE/stock-profile-bridge-v12" && \
    sha256sum -c BRIDGE_RUNTIME.sha256 >/dev/null)

require_text 'OUTPUT_MODIFIER_POLICY=ubwc' "$REPO/module/config.conf"
require_text 'DIRECT_SCANOUT=auto' "$REPO/module/config.conf"
require_text 'SWAY_THEME=dark' "$REPO/module/config.conf"
require_text 'case "$OUTPUT_MODIFIER_POLICY" in auto|linear|ubwc)' "$COMMON"
require_text 'case "$DIRECT_SCANOUT" in auto|off)' "$COMMON"
require_text 'ANDROID_DRM_OUTPUT_MODIFIER_POLICY="$OUTPUT_MODIFIER_POLICY"' "$LAUNCH"
require_text 'ANDROID_DRM_DIRECT_SCANOUT="$DIRECT_SCANOUT"' "$LAUNCH"

# Automatic mode keeps the stock wlroots decision and modifier path open.
require_text 'unset WLR_DRM_NO_MODIFIERS WLR_EGL_NO_MODIFIERS WLR_SCENE_DISABLE_DIRECT_SCANOUT' "$SESSION"
if grep -Fxq 'export WLR_DRM_NO_MODIFIERS=1' "$SESSION"; then
    fail 'stock profiles still disable modifiers unconditionally'
fi
require_text 'off) export WLR_SCENE_DISABLE_DIRECT_SCANOUT=1' "$SESSION"
require_text 'export MESA_LOADER_DRIVER_OVERRIDE=kgsl' "$SESSION"
require_text 'export FD_KGSL_ENABLE_DMABUF=1' "$SESSION"
require_text 'export FD_KGSL_ENABLE_DMABUF=1' "$KGSL_PROFILE"
require_text 'command -v xfce4-session' "$SESSION"
require_text 'export XFCE4_SESSION_COMPOSITOR=/usr/bin/xfce4-session' "$PROFILE_CLIENT"
require_text 'exec /bin/sh /etc/xdg/xfce4/xinitrc' "$PROFILE_CLIENT"
require_text '--config-dir "$xfce_labwc_dir"' "$SESSION"
if grep -Fq 'startxfce4 --wayland' "$PROFILE_CLIENT"; then
    fail 'XFCE client would launch a nested labwc compositor'
fi
if grep -Fq 'export MESA_LOADER_DRIVER_OVERRIDE=msm' "$SESSION"; then
    fail 'stock profile session must not select the upstream DRM/msm loader'
fi
require_text 'export ANDROID_DRM_PRELOAD_POLICY=append' "$SESSION"
require_text 'export ANDROID_DRM_PRELOAD_POLICY=strict-xb24-qcom-linear' "$SESSION"
require_text 'PRELOAD=$DRM_PRELOAD:$CORE:$SEAT' "$SESSION"
require_text 'lib/libandroid-drm-preload.so' "$BRIDGE/BRIDGE_PAYLOAD.sha256"
require_text 'lib/libandroid-drm-preload.so' "$BRIDGE/stock-profile-bridge-v12/BRIDGE_RUNTIME.sha256"
require_text 'lib/libandroid-drm-preload.so' "$BRIDGE/install-bridge-runtime"
require_text 'lib/libandroid-vulkan-drm-identity.so' "$BRIDGE/BRIDGE_PAYLOAD.sha256"
require_text 'lib/libandroid-vulkan-drm-identity.so' "$BRIDGE/stock-profile-bridge-v12/BRIDGE_RUNTIME.sha256"
require_text 'lib/libandroid-vulkan-drm-identity.so' "$BRIDGE/install-bridge-runtime"
require_text 'lib/libandroid-vulkan-drm-identity-layer.so' "$BRIDGE/BRIDGE_PAYLOAD.sha256"
require_text 'lib/libandroid-vulkan-drm-identity-layer.so' "$BRIDGE/stock-profile-bridge-v12/BRIDGE_RUNTIME.sha256"
require_text 'lib/libandroid-vulkan-drm-identity-layer.so' "$BRIDGE/install-bridge-runtime"
require_text 'exact_rdev_pair /dev/kgsl-3d0' "$SESSION"
require_text 'ANDROID_VULKAN_DRM_IDENTITY_ACK=$vk_identity_ack' "$SESSION"
require_text 'VK_INSTANCE_LAYERS=VK_LAYER_LINDEX_android_drm_identity' "$SESSION"
require_text 'export ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID=$$' "$SESSION"
require_text 'unset VK_LAYER_PATH VK_INSTANCE_LAYERS' "$PROFILE_CLIENT"
require_text 'unset ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID ANDROID_VULKAN_DRM_IDENTITY_TRACE' "$PROFILE_CLIENT"
require_text 'export LD_PRELOAD=$GLES_PRELOAD' "$SESSION"

require_text 'id="outputModifiers"' "$INDEX"
require_text 'id="directScanout"' "$INDEX"
require_text '<option value="turnip" data-i18n="mesaTurnipPatched">' "$INDEX"
require_text '<option value="turnip-unpatched" data-i18n="mesaTurnipUnpatched">' "$INDEX"
require_text "setValue('output_modifiers'" "$APP"
require_text "setValue('direct_scanout'" "$APP"
require_text "mesaTurnipPatched:'KGSL + 패치 Turnip · 권장'" "$LOCALES"
require_text "mesaTurnipPatched:'KGSL + patched Turnip · recommended'" "$LOCALES"
require_text "mesaTurnipUnpatched:'KGSL + 비패치 Turnip · 호환'" "$LOCALES"
require_text "mesaTurnipUnpatched:'KGSL + unpatched Turnip · compatibility'" "$LOCALES"
require_text 'standard) MESA_MODE=turnip' "$COMMON"
require_text 'turnip|turnip-unpatched|system)' "$COMMON"
require_text 'installing-kgsl-patched-turnip' "$REPO/module/payload/debian/android-drm-install"
require_text 'installing-unpatched-turnip-compatibility' "$REPO/module/payload/debian/android-drm-install"
require_text 'MESA_SELECTION_SCHEMA=2' "$REPO/module/payload/debian/android-drm-install"
require_text "outputModifiersAuto:'Automatic · compatibility first'" "$LOCALES"
require_text "outputModifiersUbwc:'UBWC preference · default'" "$LOCALES"
require_text "directScanoutAuto:'Automatic · recommended'" "$LOCALES"
require_text 'id="installProgressBar"' "$INDEX"
require_text 'id="archcraftSupport"' "$INDEX"
require_text 'id="swayThemeSetting"' "$INDEX"
require_text 'id="swayTheme"' "$INDEX"
require_text "setValue('sway_theme'" "$APP"
require_text 'updateProfileHelp();' "$APP"
require_text "swayThemeLight:'Official Light'" "$LOCALES"
require_text "swayThemePywal:'Auto-generated · Pywal'" "$LOCALES"
require_text 'href="https://ko-fi.com/s/10f2e87af3"' "$INDEX"
require_text 'target="_blank" rel="noopener noreferrer"' "$INDEX"
require_text "\$('archcraftSupport').hidden = profile !== 'sway'" "$APP"
require_text "supportArchcraft:'멋진 Sway UI를 만든 Archcraft 제작자에게 감사·후원하기'" "$LOCALES"
require_text "supportArchcraft:'Thank and support the creator of Archcraft Sway'" "$LOCALES"
require_text 'const profileNameKeys =' "$APP"
require_text 'option.textContent = nameKey ? t(nameKey) : profile.name' "$APP"
require_text "profileSway:'Archcraft Sway Free · 공식 공개 dotfiles'" "$LOCALES"
require_text "profileSway:'Archcraft Sway Free · official public dotfiles'" "$LOCALES"
require_text "lastState?.setupRunning" "$APP"
require_text "installationInProgress:'Installation is in progress." "$LOCALES"
require_text 'INSTALLATION_IN_PROGRESS' "$CONTROL"
require_text "command('profiles')" "$APP"
require_text 'ANDROID_DRM_PROGRESS_FILE=/var/lib/android-drm-lease-kit/setup-progress' "$REPO/module/bin/setup-debian-gpu"
require_text '/apps/uid_*/pid_*' "$REPO/module/bin/session-runner"
require_text '> /sys/fs/cgroup/cgroup.procs' "$REPO/module/bin/session-runner"
if grep -Eq 'lxpolkit|xfce-polkit|lxqt-policykit' \
    "$REPO/module/payload/debian/android-drm-profile-manager" \
    "$REPO/module/payload/debian/start-profile-client" \
    "$REPO/module/profiles/"*.profile; then
    fail 'unused chroot PolicyKit agent remains in the release package contract'
fi
require_text 'apt_get purge -y $obsolete_profile_packages' \
    "$REPO/module/payload/debian/android-drm-install"
require_text 'pcmanfm-qt|pcmanfm-qt' "$REPO/module/payload/debian/android-drm-profile-manager"
require_text 'adapt_sway_waybar_for_android "$target"' "$REPO/module/payload/debian/android-drm-profile-manager"
require_text '"bat": "battery"' "$REPO/module/payload/debian/android-drm-profile-manager"
require_text "-e 's/, \"network\"//g'" "$REPO/module/payload/debian/android-drm-profile-manager"
require_text 'rm -f "$1/scripts/rofi_network"' "$REPO/module/payload/debian/android-drm-profile-manager"
require_text "-e 's/, \"bluetooth\"//g'" "$REPO/module/payload/debian/android-drm-profile-manager"
require_text "-e 's/, \"backlight\"//g'" "$REPO/module/payload/debian/android-drm-profile-manager"

# Exercise the persistent WebUI contract without Android hardware. Hardware and
# DP checks fail closed, while setting validation and JSON remain testable.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/config/state" "$TMP/root"
cat > "$TMP/config/config.conf" <<EOF
AUTO_ATTACH=0
PROFILE=sway
SWAY_THEME=dark
MESA_MODE=standard
DISPLAY_MODE=auto
MODE_POLICY=preferred
OUTPUT_MODIFIER_POLICY=auto
DIRECT_SCANOUT=auto
ALLOW_XWAYLAND=0
SHARE_TOUCH=0
USB_INPUT_MODE=linux-exclusive
VIDEO_ACCELERATION=auto
RESTORE_ANDROID=1
ROOTFS=$TMP/root
SESSION_SECONDS=0
EOF

CONFIG_DIR=$TMP/config bash "$CONTROL" status-json > "$TMP/default.json"
python3 - "$TMP/default.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
assert state['outputPolicy'] == 'auto'
assert state['directScanout'] == 'auto'
assert state['profile'] == 'sway'
assert state['swayTheme'] == 'dark'
assert state['mesaMode'] == 'turnip'
assert state['installPercent'] == 0
assert state['installStage'] == 'not-installed'
PY

# A stale pre-verification marker must not make the WebUI report installed.
mkdir -p "$TMP/root/var/lib/android-drm-lease-kit/profiles"
cat > "$TMP/root/var/lib/android-drm-lease-kit/profiles/sway.ready" <<'EOF'
profile=sway
state=ready
EOF
CONFIG_DIR=$TMP/config bash "$CONTROL" status-json > "$TMP/stale-marker.json"
python3 - "$TMP/stale-marker.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
assert state['profileInstalled'] is False
PY

cat >> "$TMP/root/var/lib/android-drm-lease-kit/profiles/sway.ready" <<'EOF'
requirements=package-command-and-appearance-verified
appearance_source_commit=e4d0126d7f236fee50a84fbb0e61498dcf5705e7
appearance_archive_sha256=4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c
sway_theme=dark
EOF
CONFIG_DIR=$TMP/config bash "$CONTROL" status-json > "$TMP/verified-marker.json"
python3 - "$TMP/verified-marker.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
assert state['profileInstalled'] is True
PY

CONFIG_DIR=$TMP/config bash "$CONTROL" set output_modifiers ubwc > "$TMP/ubwc.json"
CONFIG_DIR=$TMP/config bash "$CONTROL" set direct_scanout off > "$TMP/off.json"
CONFIG_DIR=$TMP/config bash "$CONTROL" set sway_theme light > "$TMP/light.json"
grep -Fxq 'OUTPUT_MODIFIER_POLICY=ubwc' "$TMP/config/config.conf"
grep -Fxq 'DIRECT_SCANOUT=off' "$TMP/config/config.conf"
grep -Fxq 'SWAY_THEME=light' "$TMP/config/config.conf"
python3 - "$TMP/off.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
assert state['outputPolicy'] == 'ubwc'
assert state['directScanout'] == 'off'
PY

python3 - "$TMP/light.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    state = json.load(stream)
assert state['swayTheme'] == 'light'
PY

CONFIG_DIR=$TMP/config bash "$CONTROL" set sway_theme pywal > "$TMP/pywal.json"
grep -Fxq 'SWAY_THEME=pywal' "$TMP/config/config.conf"

CONFIG_DIR=$TMP/config bash "$CONTROL" set mesa turnip-unpatched > "$TMP/unpatched.json"
grep -Fxq 'MESA_MODE=turnip-unpatched' "$TMP/config/config.conf"

CONFIG_DIR=$TMP/config bash "$CONTROL" set mesa standard > "$TMP/legacy-standard.json"
grep -Fxq 'MESA_MODE=turnip' "$TMP/config/config.conf"

if CONFIG_DIR=$TMP/config bash "$CONTROL" set direct_scanout force >/dev/null 2>&1; then
    fail 'unsafe forced direct scanout value was accepted'
fi
if CONFIG_DIR=$TMP/config bash "$CONTROL" set output_modifiers invalid >/dev/null 2>&1; then
    fail 'invalid modifier policy was accepted'
fi
if CONFIG_DIR=$TMP/config bash "$CONTROL" set sway_theme custom >/dev/null 2>&1; then
    fail 'invalid Sway theme was accepted'
fi
if CONFIG_DIR=$TMP/config bash "$CONTROL" set mesa invalid >/dev/null 2>&1; then
    fail 'invalid Mesa mode was accepted'
fi

printf 'v3 WebUI, modifier bridge and unpatched direct-scanout policy tests passed\n'
