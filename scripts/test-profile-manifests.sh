#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MANAGER=$ROOT/module/payload/debian/android-drm-profile-manager

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

dash -n "$MANAGER"

for profile in sway lxqt xfce; do
    declared=$(sed -n 's/^composition=//p' "$ROOT/module/profiles/$profile.profile")
    generated=$(
        "$MANAGER" packages --profile "$profile" |
            cut -d'|' -f1 | paste -sd' ' -
    )
    [ "$declared" = "$generated" ] ||
        fail "$profile profile metadata and installer manifest differ"

    duplicates=$(
        "$MANAGER" packages --profile "$profile" |
            cut -d'|' -f1 | sort | uniq -d | paste -sd, -
    )
    [ -z "$duplicates" ] || fail "$profile has duplicate composition entries: $duplicates"

    while IFS='|' read -r upstream candidate; do
        [ -n "$upstream" ] && [ -n "$candidate" ] ||
            fail "$profile contains an incomplete manifest row"
    done < <("$MANAGER" packages --profile "$profile")
done

for expected in sway:35 lxqt:24 xfce:9; do
    profile=${expected%%:*}
    count=${expected#*:}
    actual=$("$MANAGER" packages --profile "$profile" | sed '/^$/d' | wc -l)
    [ "$actual" -eq "$count" ] ||
        fail "$profile composition count is $actual, expected $count"
done

"$MANAGER" packages --profile lxqt | grep -Fxq 'pcmanfm-qt|pcmanfm-qt' ||
    fail 'LXQt must install the PCManFM-Qt desktop/file manager'
if "$MANAGER" packages --profile lxqt | grep -Eq '^(lxqt-core|lxqt-policykit)\|'; then
    fail 'LXQt must not pull its PolicyKit agent through the core metapackage'
fi

"$MANAGER" packages --profile sway | grep -Fxq 'hyprpicker|hyprpicker' ||
    fail 'sway must install the native hyprpicker package'
"$MANAGER" packages --profile sway | grep -Fxq 'hyprlock|hyprlock' ||
    fail 'sway must install the native hyprlock package'
grep -Fq '[ "$candidate" != - ] && package_installed "$candidate" &&' "$MANAGER" ||
    fail 'native Hypr commands must require a dpkg-owned package'
grep -Fq 'remove_legacy_hypr_wrappers' "$MANAGER" ||
    fail 'legacy Hypr compatibility wrappers are not migrated'
grep -Fq '0432b6f49bff0028a86cae6e15439c460163c21769a13c95f1892c587ee74d17' "$MANAGER" ||
    fail 'legacy hyprpicker migration is not hash-scoped'
grep -Fq '03d649a8357a585369386c7016c3374aaa43d849aee0ebf1ee602dae2ae2c987' "$MANAGER" ||
    fail 'legacy hyprlock migration is not hash-scoped'

if grep -q 'compatibility command.*hypr\|swaylock "\$@"\|command -v hyprpicker.*then' "$MANAGER"; then
    fail 'obsolete Hypr compatibility wrapper remains in the installer'
fi

"$MANAGER" packages --profile sway | grep -Fxq 'light|light' ||
    fail "sway must install Debian Trixie's native light package"
if grep -Fq 'light|brightnessctl' "$MANAGER"; then
    fail 'obsolete light-to-brightnessctl substitution remains'
fi
grep -Fq 'package-command-and-appearance-verified' "$MANAGER" ||
    fail 'Sway ready markers do not record appearance verification'

# Android is the only network policy owner. The Sway profile must not launch
# NetworkManager, inspect wlan* through Waybar, or retain the upstream menu.
if grep -Eq '^[[:space:]]*run nm-applet([[:space:]]|$)' "$MANAGER"; then
    fail 'Sway runtime must not start nm-applet'
fi
grep -Fq "-e 's/, \"network\"//g'" "$MANAGER" ||
    fail 'Sway adapter does not remove the Waybar network module'
grep -Fq 'rm -f "$1/scripts/rofi_network"' "$MANAGER" ||
    fail 'Sway adapter does not remove the network-menu helper'
grep -Fq 'no-linux-network-access-v2' "$MANAGER" ||
    fail 'Sway source marker does not record the network isolation adapter'

for removed in wayfire river newm; do
    if "$MANAGER" packages --profile "$removed" >/dev/null 2>&1; then
        fail "$removed must not remain a release profile"
    fi
done

printf 'LinDeX three-profile manifest tests passed\n'
