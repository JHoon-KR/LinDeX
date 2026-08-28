#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNTIME=$ROOT/module/payload/debian/android-drm-provider-manager
PROFILE=$ROOT/module/payload/debian/android-drm-profile-manager
INSTALLER=$ROOT/module/payload/debian/android-drm-install
SETUP=$ROOT/module/bin/setup-debian-gpu
COMMON=$ROOT/module/bin/common.sh
SESSION=$ROOT/module/bin/stock-profile-session
CONTROL=$ROOT/module/bin/debian-gpu-control
CUSTOMIZE=$ROOT/module/customize.sh
PACKAGER=$ROOT/scripts/package-v3-module.ps1
VERIFIER=$ROOT/scripts/verify-v3-module.ps1
INDEX=$ROOT/module/webroot/index.html

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
require_line() { grep -Fq -- "$1" "$2" || fail "missing '$1' in ${2#$ROOT/}"; }

dash -n "$RUNTIME" "$PROFILE" "$INSTALLER" "$CUSTOMIZE"
bash -n "$SETUP" "$COMMON" "$SESSION" "$CONTROL"

require_line '/usr/local/libexec/android-drm/profile-configurator' "$INSTALLER"
require_line '/usr/local/libexec/android-drm/profile-runtime-manager' "$PROFILE"
require_line 'profile-configurator' "$CUSTOMIZE"
require_line 'profile-runtime-manager' "$CUSTOMIZE"
require_line 'PROFILE=sway' "$COMMON"
require_line 'SWAY_THEME=dark' "$COMMON"
require_line 'ANDROID_DRM_SWAY_THEME="$SWAY_THEME"' "$SETUP"
require_line 'sway|lxqt|xfce' "$PROFILE"
require_line 'archcraft-official-public-dotfiles' "$PROFILE"
require_line 'package-command-and-appearance-verified' "$PROFILE"
require_line 'hyprpicker|hyprpicker' "$PROFILE"
require_line 'light|light' "$PROFILE"
require_line 'wofi|wofi' "$PROFILE"
require_line 'kanshi|kanshi' "$PROFILE"
require_line 'wlogout|wlogout' "$PROFILE"
require_line 'profiles/sway.profile' "$VERIFIER"
require_line 'Release requires a pinned Sway source/public-asset archive and digest' "$PACKAGER"
require_line 'Official Archcraft Sway asset digest mismatch' "$VERIFIER"
require_line 'PROVIDER: pastel | python-pywal' "$RUNTIME"

for removed in river-stack newm-next-stack zig-0.16; do
    if grep -Fq -- "$removed" "$RUNTIME"; then
        fail "removed provider remains in runtime manager: $removed"
    fi
done

require_line 'archcraft-sway-free-e4d0126d.tar.gz' "$CUSTOMIZE"
require_line 'lindex-archcraft-sway-public-assets-v2.tar.gz' "$SETUP"
require_line 'apply_official_sway_theme' "$PROFILE"
require_line '"bat": "battery"' "$PROFILE"
require_line 's/, "network"//g' "$PROFILE"
require_line 'rm -f "$1/scripts/rofi_network"' "$PROFILE"
require_line 'scripts/android_battery' "$PROFILE"
require_line '"custom/android-battery"' "$PROFILE"
require_line 'scripts/android_volume' "$PROFILE"
require_line 'scripts/android_volume_control' "$PROFILE"
require_line '"custom/android-volume"' "$PROFILE"
require_line 'on-scroll-up' "$PROFILE"
require_line 'on-scroll-down' "$PROFILE"
require_line 'STREAM_MUSIC' "$PROFILE"
require_line 'android-volume-v1' "$PROFILE"
require_line 'fa4-lightmode-v1' "$PROFILE"
require_line 's/󰖨//g' "$PROFILE"
require_line 'charge_now=0' "$PROFILE"
require_line '"FontAwesome"' "$PROFILE"
require_line 'process_android_volume_commands' "$ROOT/module/bin/auto-service"
require_line 'android_media_volume_command --get --stream 3' "$ROOT/module/bin/auto-service"
require_line 'ANDROID_MEDIA_TIMEOUT=${ANDROID_MEDIA_TIMEOUT:-/system/bin/timeout}' "$ROOT/module/bin/auto-service"
require_line 'chmod 0700 "$ANDROID_VOLUME_RUNTIME" "$ANDROID_VOLUME_COMMANDS"' "$ROOT/module/bin/auto-service"
if grep -Fq 'run mpd' "$PROFILE"; then
    fail 'Sway startup must not launch a Linux audio daemon'
fi
require_line 'source_path=archcraft-icons-breeze/files/Archcraft' "$ROOT/module/profile-assets/APPEARANCE_SOURCES.lock"

for no_polkit in "$PROFILE" \
    "$ROOT/module/payload/debian/start-profile-client" \
    "$ROOT/module/profiles/sway.profile" \
    "$ROOT/module/profiles/lxqt.profile" \
    "$ROOT/module/profiles/xfce.profile"; do
    if grep -Eq '(^|[^[:alnum:]_-])(lxpolkit|xfce-polkit|lxqt-policykit)([^[:alnum:]_-]|$)' "$no_polkit"; then
        fail "unused chroot PolicyKit agent remains in ${no_polkit#$ROOT/}"
    fi
done

require_line 'for obsolete_package in lxpolkit xfce-polkit lxqt-policykit lxqt-core; do' "$INSTALLER"
require_line 'apt_get purge -y $obsolete_profile_packages' "$INSTALLER"

require_line 'pcmanfm-qt|pcmanfm-qt' "$PROFILE"
require_line 'pcmanfm-qt' "$ROOT/module/profiles/lxqt.profile"
if grep -Fq -- 'lxqt-core' "$ROOT/module/profiles/lxqt.profile" ||
   grep -Fq -- 'lxqt-core|lxqt-core' "$PROFILE"; then
    fail 'LXQt metapackage would pull the unused chroot PolicyKit agent'
fi

profiles=$(sed -n '/<select id="profile">/,/<\/select>/p' "$INDEX" |
    sed -n 's/.*<option value="\([^"]*\)".*/\1/p' | tr '\n' ' ')
[ "$profiles" = 'sway lxqt xfce ' ] || \
    fail "unexpected WebUI profile list: $profiles"

printf 'LinDeX official Sway profile contract tests passed\n'
