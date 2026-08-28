#!/usr/bin/env bash
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
HARNESS=$REPO/scripts/lindex-graphics-device-gate.sh
TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT HUP INT TERM

fail() {
    printf 'graphics device gate fixture: FAIL: %s\n' "$*" >&2
    exit 1
}

STATE=$TMP/state
GATE=$TMP/gate
PROC=$TMP/proc
ROOTFS=$TMP/rootfs
MOCK=$TMP/mock
mkdir -p "$STATE" "$GATE" "$PROC" "$ROOTFS/run/user/0" "$MOCK"
: > "$STATE/session.log"
printf 'lxqt\n' > "$MOCK/profile"
printf '0\n' > "$MOCK/auto-attach"
printf 'stopped\n' > "$MOCK/mode"
printf 'ON\n' > "$MOCK/external-state"
: > "$MOCK/control.log"

write_process() {
    fixture_pid=$1
    fixture_start=$2
    mkdir -p "$PROC/$fixture_pid"
    awk -v pid="$fixture_pid" -v start="$fixture_start" 'BEGIN {
        printf "%s (fixture) S", pid
        for (field = 4; field <= 21; field++) printf " 0"
        printf " %s 0\n", start
    }' > "$PROC/$fixture_pid/stat"
}

write_running_markers() {
    fixture_pid=$1
    fixture_pgid=$2
    fixture_pid_start=$3
    fixture_pgid_start=$4
    printf '%s\n' "$fixture_pid" > "$STATE/session.pid"
    printf '%s lindex-fixture-%s-%s\n' \
        "$fixture_pgid" "$fixture_pid_start" "$fixture_pgid_start" > \
        "$STATE/session.pgid"
    printf 'running\n' > "$STATE/session-state"
    printf '18\n' > "$STATE/last_dp_display_id"
    write_process "$fixture_pid" "$fixture_pid_start"
    write_process "$fixture_pgid" "$fixture_pgid_start"
}

cat > "$MOCK/control" <<'EOF'
#!/bin/sh
set -eu

write_stat() {
    pid=$1
    start=$2
    mkdir -p "$MOCK_PROC/$pid"
    awk -v pid="$pid" -v start="$start" 'BEGIN {
        printf "%s (fixture) S", pid
        for (field = 4; field <= 21; field++) printf " 0"
        printf " %s 0\n", start
    }' > "$MOCK_PROC/$pid/stat"
}

status_json() {
    profile=$(cat "$MOCK_DIR/profile")
    auto_attach=$(cat "$MOCK_DIR/auto-attach")
    mode=$(cat "$MOCK_DIR/mode")
    case "$mode" in
        running|reconnected)
            dp=connected; edid=ready; running=true
            pid=$(cat "$MOCK_STATE/session.pid")
            ;;
        unplugged)
            dp=disconnected; edid=disconnected; running=false; pid=0
            ;;
        adapter)
            dp=adapter; edid=waiting; running=false; pid=0
            ;;
        stopped)
            dp=connected; edid=ready; running=false; pid=0
            ;;
        *) exit 65 ;;
    esac
    printf '{"hardware":"ready","dp":"%s","dpPath":"/sys/class/drm/card0-DP-1","edidState":"%s","running":%s,"pid":%s,"setupRunning":false,"preparationState":"complete","preparationDetail":"session-started","installPercent":100,"installStage":"complete","installDetail":"profile-and-mesa-ready","lastError":"none","autoAttach":%s,"profile":"%s","profileInstalled":true,"swayTheme":"dark","mesaMode":"turnip","displayMode":"auto","modePolicy":"preferred","outputPolicy":"auto","directScanout":"auto","xwayland":false,"shareTouch":false,"usbInputMode":"linux-exclusive","videoAcceleration":"auto","bridgeReady":true,"buildFlavor":"dev","logsEnabled":true,"rootfs":"%s"}\n' \
        "$dp" "$edid" "$running" "$pid" "$auto_attach" "$profile" \
        "$MOCK_ROOTFS"
}

printf '%s\n' "$*" >> "$MOCK_DIR/control.log"
case "${1:-}" in
    status-json)
        status_json
        ;;
    set)
        case "${2:-}" in
            profile) printf '%s\n' "$3" > "$MOCK_DIR/profile" ;;
            auto_attach) printf '%s\n' "$3" > "$MOCK_DIR/auto-attach" ;;
        esac
        status_json
        ;;
    start-ready-async)
        printf 'running\n' > "$MOCK_DIR/mode"
        printf '101\n' > "$MOCK_STATE/session.pid"
        printf '201 lindex-fixture-1001-2001\n' > "$MOCK_STATE/session.pgid"
        printf 'running\n' > "$MOCK_STATE/session-state"
        printf '18\n' > "$MOCK_STATE/last_dp_display_id"
        write_stat 101 1001
        write_stat 201 2001
        printf 'renderer=vulkan icd=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json\n' >> "$MOCK_STATE/session.log"
        printf 'profile preparation accepted pid=99\n'
        ;;
    stop)
        rm -rf -- "$MOCK_PROC/101" "$MOCK_PROC/201" "$MOCK_PROC/102" "$MOCK_PROC/202"
        rm -f "$MOCK_STATE/session.pid" "$MOCK_STATE/session.pgid" \
            "$MOCK_STATE/session-start.intent"
        printf 'stopped\n' > "$MOCK_STATE/session-state"
        printf 'stopped\n' > "$MOCK_DIR/mode"
        printf 'ON\n' > "$MOCK_DIR/external-state"
        ;;
    dev-log)
        cat "$MOCK_STATE/session.log"
        ;;
    *) exit 64 ;;
esac
EOF
chmod 0700 "$MOCK/control"

cat > "$MOCK/cmd" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = display ]
[ "$2" = get-displays ]
state=$(cat "$MOCK_DIR/external-state")
[ "$state" != DISCONNECTED ] || exit 0
printf 'Display id 18: DisplayInfo{"Fixture", displayId 18, state %s, committedState %s, type EXTERNAL}\n' \
    "$state" "$state"
EOF

cat > "$MOCK/dumpsys" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = display ]
state=$(cat "$MOCK_DIR/external-state")
[ "$state" != DISCONNECTED ] || exit 0
printf 'mDisplayInfo=DisplayInfo{"Fixture", displayId 18, state %s, committedState %s, type EXTERNAL}\n' \
    "$state" "$state"
EOF
chmod 0700 "$MOCK/cmd" "$MOCK/dumpsys"

export MOCK_DIR=$MOCK
export MOCK_STATE=$STATE
export MOCK_PROC=$PROC
export MOCK_ROOTFS=$ROOTFS
export LINDEX_CONTROL=$MOCK/control
export LINDEX_STATE_DIR=$STATE
export LINDEX_GATE_ROOT=$GATE
export LINDEX_PROC_ROOT=$PROC
export LINDEX_CMD_TOOL=$MOCK/cmd
export LINDEX_DUMPSYS_TOOL=$MOCK/dumpsys
export LINDEX_SLEEP_TOOL=true
export LINDEX_START_WAIT_SECONDS=2
export LINDEX_STOP_WAIT_SECONDS=2
export LINDEX_POLL_SECONDS=1

run_pass() {
    phase=$1
    profile=$2
    output=$(dash "$HARNESS" "$phase" "$profile") ||
        fail "$phase $profile did not return PASS"
    printf '%s\n' "$output" | grep -F 'result=PASS' >/dev/null ||
        fail "$phase $profile did not record PASS"
}

run_fail() {
    phase=$1
    profile=$2
    set +e
    output=$(dash "$HARNESS" "$phase" "$profile" 2>&1)
    rc=$?
    set -e
    [ "$rc" -eq 1 ] || fail "$phase $profile returned $rc instead of FAIL"
    printf '%s\n' "$output" | grep -F 'result=FAIL' >/dev/null ||
        fail "$phase $profile did not record FAIL"
}

run_indeterminate() {
    phase=$1
    profile=$2
    set +e
    output=$(dash "$HARNESS" "$phase" "$profile" 2>&1)
    rc=$?
    set -e
    [ "$rc" -eq 2 ] ||
        fail "$phase $profile returned $rc instead of INDETERMINATE"
    printf '%s\n' "$output" | grep -F 'result=INDETERMINATE' >/dev/null ||
        fail "$phase $profile did not record INDETERMINATE"
}

dash -n "$HARNESS"
grep -F 'swaymsg -s "$sway_socket_guest"' "$HARNESS" >/dev/null ||
    fail 'same-lease observer is not launched through the live Sway process'
grep -F 'ANDROID_DRM_PRELOAD_PRIMARY_PLANE' "$HARNESS" >/dev/null ||
    fail 'GETFB2 observer does not use the inherited primary-plane identity'
if grep -Eq 'disable-display|enable-display|power-reset|cable_connect_ctrl' "$HARNESS"; then
    fail 'device gate contains a forbidden physical/display manipulation'
fi
if grep -R -F 'lindex-graphics-device-gate' "$REPO/module" >/dev/null 2>&1; then
    fail 'source-only device gate leaked into the packaged module tree'
fi

run_pass start lxqt
run_pass status lxqt
expected_start_sequence='stop
set auto_attach 0
set profile lxqt
set output_modifiers auto
set direct_scanout auto
set mode_policy preferred
set display_mode auto
start-ready-async'
actual_start_sequence=$(sed -n '1,8p' "$MOCK/control.log")
[ "$actual_start_sequence" = "$expected_start_sequence" ] ||
    fail 'start did not disable auto-attach before changing profile settings'

run_pass before-unplug lxqt
printf 'unplugged\n' > "$MOCK/mode"
printf 'DISCONNECTED\n' > "$MOCK/external-state"
run_fail after-unplug lxqt

rm -rf -- "$PROC/101" "$PROC/201"
rm -f "$STATE/session.pid" "$STATE/session.pgid" "$STATE/session-start.intent"
printf 'stopped\n' > "$STATE/session-state"
run_pass after-unplug lxqt

printf 'reconnected\n' > "$MOCK/mode"
printf 'ON\n' > "$MOCK/external-state"
write_running_markers 102 202 1002 2002
printf 'renderer=gles2 reason=vulkan-unavailable-or-software\n' >> "$STATE/session.log"
run_pass after-reconnect lxqt
grep -F 'manual_visual_confirmation=required' \
    "$GATE/lxqt/after-reconnect-result.txt" >/dev/null ||
    fail 'reconnect machine PASS did not retain the manual visual requirement'
run_pass stop lxqt

# Re-enter a Sway session and feed direct-check the exact bounded artifacts
# that direct-start creates. This exercises classification without requiring
# a real compositor or a physical connector in the host fixture.
run_pass start sway
DIRECT=$GATE/sway
RUNTIME=$ROOTFS/run/user/0
baseline=$(wc -l < "$STATE/session.log" | tr -d ' ')
session_identity=101:1001
group_identity=201:2001
cat > "$DIRECT/direct.state" <<EOF
profile=sway
rootfs=$ROOTFS
session_identity=$session_identity
group_identity=$group_identity
session_log_baseline=$baseline
getfb2_host=$RUNTIME/lindex-gate-getfb2.log
observer_done_host=$RUNTIME/lindex-gate-getfb2.done
vkmark_host=$RUNTIME/lindex-gate-vkmark.log
vkmark_done_host=$RUNTIME/lindex-gate-vkmark.done
tree_host=$RUNTIME/lindex-gate-sway-tree.json
tree_proof_host=$RUNTIME/lindex-gate-sway-tree.proof
EOF
printf 'LEASE_GETFB2 plane=126 crtc=281 fb=41 size=1920x1080 format=0x34324258 modifier=0x0500000000000001\nLEASE_GETFB2 result=UBWC_PASS exact=XB24/0x0500000000000001\n' > \
    "$RUNTIME/lindex-gate-getfb2.log"
printf '120\n' > "$RUNTIME/lindex-gate-getfb2.done"
printf 'Vulkan device: Fixture\nvkmark Score: 12345\n' > \
    "$RUNTIME/lindex-gate-vkmark.log"
printf '0\n' > "$RUNTIME/lindex-gate-vkmark.done"
printf 'VKMARK_TREE matches=1 fullscreen=1 exact_output=1\n' > \
    "$RUNTIME/lindex-gate-sway-tree.proof"
printf '[fixture] Direct scan-out enabled\n' >> "$STATE/session.log"

rm -f "$RUNTIME/lindex-gate-getfb2.done"
run_indeterminate direct-check sway
printf '120\n' > "$RUNTIME/lindex-gate-getfb2.done"
run_pass direct-check sway

sed -i '$d' "$STATE/session.log"
run_fail direct-check sway
printf '[fixture] Direct scan-out enabled\n' >> "$STATE/session.log"

printf 'LEASE_GETFB2 plane=126 crtc=281 fb=42 size=1920x1080 format=0x34324241 modifier=0x0500000000000001\nLEASE_GETFB2 result=INDETERMINATE reason=unexpected-format-modifier format=0x34324241 modifier=0x0500000000000001\n' > \
    "$RUNTIME/lindex-gate-getfb2.log"
run_indeterminate direct-check sway

printf 'LinDeX source-only graphics device gate fixtures: PASS\n'
