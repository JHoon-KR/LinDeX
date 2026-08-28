#!/usr/bin/env bash
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
GROUP_PID=
EDID_WRITER_PID=
AUTO_TEST_PID=
INTENT_SESSION_PID=
ORPHAN_CHILD_PID=

cleanup() {
    if [[ ${EDID_WRITER_PID:-} =~ ^[0-9]+$ ]] && kill -0 "$EDID_WRITER_PID" 2>/dev/null; then
        kill "$EDID_WRITER_PID" 2>/dev/null || true
        wait "$EDID_WRITER_PID" 2>/dev/null || true
    fi
    if [[ ${GROUP_PID:-} =~ ^[0-9]+$ ]] && kill -0 "$GROUP_PID" 2>/dev/null; then
        kill -TERM -- "-$GROUP_PID" 2>/dev/null || true
        wait "$GROUP_PID" 2>/dev/null || true
    fi
    if [[ ${AUTO_TEST_PID:-} =~ ^[0-9]+$ ]] && kill -0 "$AUTO_TEST_PID" 2>/dev/null; then
        kill -TERM "$AUTO_TEST_PID" 2>/dev/null || true
        wait "$AUTO_TEST_PID" 2>/dev/null || true
    fi
    if [[ ${INTENT_SESSION_PID:-} =~ ^[0-9]+$ ]] && kill -0 "$INTENT_SESSION_PID" 2>/dev/null; then
        kill -TERM "$INTENT_SESSION_PID" 2>/dev/null || true
        wait "$INTENT_SESSION_PID" 2>/dev/null || true
    fi
    if [[ ${ORPHAN_CHILD_PID:-} =~ ^[0-9]+$ ]] && kill -0 "$ORPHAN_CHILD_PID" 2>/dev/null; then
        kill -KILL "$ORPHAN_CHILD_PID" 2>/dev/null || true
    fi
    rm -rf -- "$TMP"
}
trap cleanup EXIT HUP INT TERM

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# The v3 watcher deliberately does not pulse Type-C CC or attempt autonomous
# link recovery. It only reacts to a physically valid DP+EDID state, owns one
# session process group, and tears that group down after a debounced unplug.
if grep -Eq 'request_typec_dp_renegotiation|cable_connect_ctrl|display-link-cc-open' \
        "$REPO/module/bin/auto-service" "$REPO/module/bin/common.sh"; then
    fail 'v3 reintroduced forbidden automatic Type-C link recovery'
fi
grep -F 'absent_samples=$((absent_samples + 1))' \
    "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'unplug debounce counter is missing'
grep -F '[ "$raw_connected" = 1 ] || stop_threshold=1' \
    "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'hard-disconnect first-sample stop is missing'
grep -F 'stop_threshold=2' "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'transient EDID debounce is missing'
grep -F '[ "$present_samples" -ge 2 ]' \
    "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'stable reconnect gate is missing'
grep -F 'stop_desktop' "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'unplug path does not stop the owned desktop'
grep -F 'if [ "$connected" = 1 ]' \
    "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'reconnect path does not observe valid physical DP state'
grep -F 'session_start_intent_active "$now"' \
    "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'watcher does not recognize bounded intentional pre-detach'
grep -F 'typec_partner_present' "$REPO/module/bin/auto-service" >/dev/null ||
    fail 'pre-detach grace does not require a retained physical Type-C partner'
grep -F 'clear_session_start_intent "${SESSION_START_OWNER_PID:-}"' \
    "$REPO/module/bin/launch-stock-profile" >/dev/null ||
    fail 'established lease client does not clear pre-detach intent'

export DRM_CLASS="$TMP/drm"
mkdir -p "$DRM_CLASS/card0-DP-1"
printf 'disconnected\n' > "$DRM_CLASS/card0-DP-1/status"
: > "$DRM_CLASS/card0-DP-1/edid"

# shellcheck source=/dev/null
. "$REPO/module/bin/common.sh"

if physical_dp_path >/dev/null 2>&1; then
    fail 'disconnected connector was accepted'
fi
printf 'connected\n' > "$DRM_CLASS/card0-DP-1/status"
if physical_dp_path >/dev/null 2>&1; then
    fail 'connected connector without EDID was accepted'
fi

# A sysfs attribute reports st_size=0 even while a read returns its payload.
# A FIFO has the same metadata behavior and guards against using `-s` here.
rm -f "$DRM_CLASS/card0-DP-1/edid"
mkfifo "$DRM_CLASS/card0-DP-1/edid"
dd if=/dev/zero of="$DRM_CLASS/card0-DP-1/edid" bs=256 count=1 status=none &
EDID_WRITER_PID=$!
expected_connector="$DRM_CLASS/card0-DP-1"
actual_connector=$(physical_dp_path) || fail 'zero-size sysfs-style EDID was rejected'
wait "$EDID_WRITER_PID"
EDID_WRITER_PID=
[ "$actual_connector" = "$expected_connector" ] ||
    fail 'wrong physical connector was selected'
rm -f "$DRM_CLASS/card0-DP-1/edid"
dd if=/dev/zero of="$DRM_CLASS/card0-DP-1/edid" bs=128 count=1 status=none
printf 'disconnected\n' > "$DRM_CLASS/card0-DP-1/status"
if physical_dp_path >/dev/null 2>&1; then
    fail 'unplug transition kept stale DP state'
fi
printf 'connected\n' > "$DRM_CLASS/card0-DP-1/status"
actual_connector=$(physical_dp_path) || fail 'replug transition was not detected'
[ "$actual_connector" = "$expected_connector" ] ||
    fail 'replug selected the wrong connector'

# Exercise exact process-group ownership using a disposable session-shaped
# group. No process-name-wide signal is used.
MODDIR="$TMP/module"
STATE="$TMP/state"
PIDFILE="$STATE/session.pid"
mkdir -p "$MODDIR/bin" "$STATE"
fixture="$MODDIR/bin/launch-stock-profile"
printf '%s\n' '#!/bin/sh' \
    'trap "exit 0" HUP INT TERM' \
    'while :; do sleep 1; done' > "$fixture"
chmod 0700 "$fixture"
GROUP_TOKEN=lindex-test-$$
LINDEX_SESSION_TOKEN=$GROUP_TOKEN setsid "$fixture" >/dev/null 2>&1 &
GROUP_PID=$!
sleep 0.2
printf '%s %s\n' "$GROUP_PID" "$GROUP_TOKEN" > "$STATE/session.pgid"
recorded_session_group_running || fail 'owned session group was not recognized'
stop_recorded_session_group
wait "$GROUP_PID" 2>/dev/null || true
if kill -0 "$GROUP_PID" 2>/dev/null; then
    fail 'owned session group survived unplug cleanup'
fi
[ ! -e "$STATE/session.pgid" ] || fail 'stale session group marker survived cleanup'
GROUP_PID=

# The launcher leader may exit on TERM before a compositor child releases its
# DRM fd. Token-authenticated cleanup must still find and KILL that remaining
# child instead of dropping the PGID marker with an orphan alive.
cat > "$fixture" <<'EOF'
#!/bin/sh
trap 'exit 0' HUP INT TERM
(
    trap '' HUP INT TERM
    while :; do sleep 1; done
) &
printf '%s\n' "$!" > "$CHILD_RECORD"
wait
EOF
chmod 0700 "$fixture"
GROUP_TOKEN=lindex-orphan-$$
CHILD_RECORD=$TMP/orphan-child \
LINDEX_SESSION_TOKEN=$GROUP_TOKEN \
    setsid "$fixture" >/dev/null 2>&1 &
GROUP_PID=$!
ORPHAN_GROUP_PID=$GROUP_PID
for _ in 1 2 3 4 5; do
    [ -s "$TMP/orphan-child" ] && break
    sleep 0.1
done
ORPHAN_CHILD_PID=$(cat "$TMP/orphan-child" 2>/dev/null) ||
    fail 'orphan cleanup fixture did not start its compositor child'
printf '%s %s\n' "$GROUP_PID" "$GROUP_TOKEN" > "$STATE/session.pgid"
SESSION_GROUP_TERM_TIMEOUT=1
stop_recorded_session_group
unset SESSION_GROUP_TERM_TIMEOUT
wait "$ORPHAN_GROUP_PID" 2>/dev/null || true
GROUP_PID=
if owned_process_group_running "$ORPHAN_GROUP_PID" "$GROUP_TOKEN"; then
    fail 'token-owned compositor child survived leader-first cleanup'
fi
ORPHAN_CHILD_PID=

# A recorded PID with the wrong command identity must never be signalled.
sleep 30 &
foreign_pid=$!
printf '%s\n' "$foreign_pid" > "$STATE/session.pgid"
if recorded_session_group_running; then
    kill "$foreign_pid" 2>/dev/null || true
    wait "$foreign_pid" 2>/dev/null || true
    fail 'foreign process was accepted as a LinDeX session group'
fi
stop_recorded_session_group
kill -0 "$foreign_pid" 2>/dev/null || fail 'foreign process was signalled'
kill "$foreign_pid"
wait "$foreign_pid" 2>/dev/null || true

# Reproduce the startup race: DisplayManager pre-detach makes DP/EDID absent
# for more than two watcher samples. An exact, fresh session-runner intent must
# protect only that bounded interval; an expired marker must fail safe into the
# normal owned-session unplug cleanup.
intent_config=$TMP/intent-config
intent_state_dir=$intent_config/state
intent_drm=$TMP/intent-drm
intent_typec=$TMP/intent-typec
mkdir -p "$intent_state_dir" "$intent_drm/card0-DP-1" \
    "$intent_typec/port0-partner" "$TMP/intent-root"
printf 'disconnected\n' > "$intent_drm/card0-DP-1/status"
: > "$intent_drm/card0-DP-1/edid"
cat > "$intent_config/config.conf" <<EOF
AUTO_ATTACH=0
PROFILE=sway
MESA_MODE=turnip
DISPLAY_MODE=auto
MODE_POLICY=preferred
OUTPUT_MODIFIER_POLICY=auto
DIRECT_SCANOUT=auto
ALLOW_XWAYLAND=0
SHARE_TOUCH=0
USB_INPUT_MODE=linux-exclusive
VIDEO_ACCELERATION=disabled
RESTORE_ANDROID=1
ROOTFS=$TMP/intent-root
SESSION_SECONDS=0
EOF

bash -c 'trap "exit 0" HUP INT TERM; while :; do sleep 1; done' \
    "$REPO/module/bin/session-runner" \
    "$REPO/module/bin/launch-stock-profile" >/dev/null 2>&1 &
INTENT_SESSION_PID=$!
printf '%s\n' "$INTENT_SESSION_PID" > "$intent_state_dir/session.pid"
printf 'starting\n' > "$intent_state_dir/session-state"
intent_now=$(date +%s)
printf '%s %s\n' "$INTENT_SESSION_PID" "$intent_now" > \
    "$intent_state_dir/session-start.intent"

MODDIR=$REPO/module
STATE=$intent_state_dir
PIDFILE=$intent_state_dir/session.pid
session_start_intent_active "$intent_now" ||
    fail 'fresh exact-PID pre-detach intent was rejected'
if clear_session_start_intent "$((INTENT_SESSION_PID + 1))"; then
    fail 'mismatched PID cleared pre-detach intent'
fi
[ -e "$intent_state_dir/session-start.intent" ] ||
    fail 'mismatched clear removed pre-detach intent'
printf '%s %s\n' "$$" "$intent_now" > "$intent_state_dir/session-start.intent"
printf '%s\n' "$$" > "$intent_state_dir/session.pid"
if session_start_intent_active "$intent_now"; then
    fail 'foreign command identity activated pre-detach grace'
fi
printf '%s\n' "$INTENT_SESSION_PID" > "$intent_state_dir/session.pid"
printf '%s %s\n' "$INTENT_SESSION_PID" "$intent_now" > \
    "$intent_state_dir/session-start.intent"

CONFIG_DIR=$intent_config DRM_CLASS=$intent_drm TYPEC_CLASS=$intent_typec \
    bash "$REPO/module/bin/auto-service" >/dev/null 2>&1 &
AUTO_TEST_PID=$!
sleep 3
kill -0 "$INTENT_SESSION_PID" 2>/dev/null ||
    fail 'watcher killed the session during intentional pre-detach grace'

clear_session_start_intent "$INTENT_SESSION_PID" ||
    fail 'exact owner could not clear pre-detach intent'
[ ! -e "$intent_state_dir/session-start.intent" ] ||
    fail 'cleared pre-detach marker survived'
printf '%s %s\n' "$INTENT_SESSION_PID" "$((intent_now - 16))" > \
    "$intent_state_dir/session-start.intent"
sleep 3
if kill -0 "$INTENT_SESSION_PID" 2>/dev/null; then
    fail 'expired pre-detach intent suppressed forced-unplug cleanup'
fi
wait "$INTENT_SESSION_PID" 2>/dev/null || true
INTENT_SESSION_PID=
[ ! -e "$intent_state_dir/session-start.intent" ] ||
    fail 'expired pre-detach marker survived cleanup'
[ ! -e "$intent_state_dir/session.pid" ] ||
    fail 'expired-intent cleanup left the session PID marker'
[ "$(cat "$intent_state_dir/session-state" 2>/dev/null)" = stopped ] ||
    fail 'expired-intent cleanup did not stop the owned session'

kill -TERM "$AUTO_TEST_PID" 2>/dev/null || true
wait "$AUTO_TEST_PID" 2>/dev/null || true
AUTO_TEST_PID=

# A fresh startup marker must not hide a real cable removal. Once the Type-C
# partner disappears, the same disconnected sysfs state stops on the first
# watcher sample rather than waiting for the 15-second startup grace.
rmdir "$intent_typec/port0-partner"
bash -c 'trap "exit 0" HUP INT TERM; while :; do sleep 1; done' \
    "$REPO/module/bin/session-runner" \
    "$REPO/module/bin/launch-stock-profile" >/dev/null 2>&1 &
INTENT_SESSION_PID=$!
printf '%s\n' "$INTENT_SESSION_PID" > "$intent_state_dir/session.pid"
printf 'starting\n' > "$intent_state_dir/session-state"
intent_now=$(date +%s)
printf '%s %s\n' "$INTENT_SESSION_PID" "$intent_now" > \
    "$intent_state_dir/session-start.intent"
CONFIG_DIR=$intent_config DRM_CLASS=$intent_drm TYPEC_CLASS=$intent_typec \
    bash "$REPO/module/bin/auto-service" >/dev/null 2>&1 &
AUTO_TEST_PID=$!
sleep 2
if kill -0 "$INTENT_SESSION_PID" 2>/dev/null; then
    fail 'fresh startup intent hid a physical Type-C partner removal'
fi
wait "$INTENT_SESSION_PID" 2>/dev/null || true
INTENT_SESSION_PID=
kill -TERM "$AUTO_TEST_PID" 2>/dev/null || true
wait "$AUTO_TEST_PID" 2>/dev/null || true
AUTO_TEST_PID=

# A soft restart can leave a stale "running" state after the exact runner and
# process-group markers are already gone.  Disconnected reconciliation must
# correct the WebUI state without requiring a process to signal.
rm -f "$intent_state_dir/session.pid" "$intent_state_dir/session.pgid" \
    "$intent_state_dir/session-start.intent"
printf 'running\n' > "$intent_state_dir/session-state"
CONFIG_DIR=$intent_config DRM_CLASS=$intent_drm \
    bash "$REPO/module/bin/auto-service" >/dev/null 2>&1 &
AUTO_TEST_PID=$!
sleep 3
[ "$(cat "$intent_state_dir/session-state" 2>/dev/null)" = stopped ] ||
    fail 'disconnected watcher left a stale running WebUI state'
kill -TERM "$AUTO_TEST_PID" 2>/dev/null || true
wait "$AUTO_TEST_PID" 2>/dev/null || true
AUTO_TEST_PID=

printf 'v3 hotplug cleanup and reconnect tests: PASS\n'
