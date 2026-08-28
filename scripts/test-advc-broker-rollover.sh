#!/usr/bin/env bash
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SERVICE="$REPO/module/bin/advc-broker-service"

# KernelSU replaces the module file before the old process exits, so Android
# procfs reports the executable as "PATH (deleted)".  Keep the normalization
# semantics exercised independently of the fixture symlink implementation.
deleted_exe='/data/adb/modules/debian_chroot/bin/advc-broker (deleted)'
case "$deleted_exe" in
	*' (deleted)') normalized_exe=${deleted_exe% *} ;;
	*) normalized_exe=$deleted_exe ;;
esac
[ "$normalized_exe" = /data/adb/modules/debian_chroot/bin/advc-broker ] || {
	echo 'deleted executable suffix normalization failed' >&2
	exit 1
}
grep -Fq "*' (deleted)') identity_exe_arg=" "$SERVICE" || {
	echo 'broker service lacks deleted executable normalization' >&2
	exit 1
}

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

make_socket_node() {
    socket_path=$1
    rm -f "$socket_path"
    python3 - "$socket_path" <<'PY'
import socket
import sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.bind(sys.argv[1])
s.close()
PY
}

write_net_unix() {
    proc_root=$1
    inode=$2
    socket_path=$3
    cat > "$proc_root/net/unix" <<EOF
Num       RefCount Protocol Flags    Type St Inode Path
00000000: 00000002 00000000 00010000 0001 01 $inode $socket_path
EOF
}

clear_net_unix() {
    cat > "$1/net/unix" <<'EOF'
Num       RefCount Protocol Flags    Type St Inode Path
EOF
}

make_proc() {
    proc_root=$1
    pid=$2
    exe=$3
    socket_path=$4
    inode=$5
    start_time=$6
    mkdir -p "$proc_root/$pid/fd"
    ln -s "$exe" "$proc_root/$pid/exe"
    printf 'Uid:\t0\t0\t0\t0\n' > "$proc_root/$pid/status"
    printf '%s (advc-broker) S 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %s 0\n' \
        "$pid" "$start_time" > "$proc_root/$pid/stat"
    printf '%s\0%s\0' "$exe" "$socket_path" > "$proc_root/$pid/cmdline"
    ln -s "socket:[$inode]" "$proc_root/$pid/fd/3"
}

setup_case() {
    name=$1
    CASE_DIR="$TMP/$name"
    PROC_ROOT="$CASE_DIR/proc"
    STATE_DIR="$CASE_DIR/state"
    MODULE_DIR="$CASE_DIR/module"
    ACTIVE="$MODULE_DIR/bin/advc-broker"
    LEGACY="$CASE_DIR/legacy/advc-broker-1.1"
    FOREIGN="$CASE_DIR/foreign/not-advc"
    SOCKET="$CASE_DIR/run/advc-broker-1.1.sock"
    SIGNALS="$CASE_DIR/signals"
    LAUNCHES="$CASE_DIR/launches"
    mkdir -p "$PROC_ROOT/net" "$STATE_DIR" "$MODULE_DIR/bin" \
        "$CASE_DIR/legacy" "$CASE_DIR/foreign" "$CASE_DIR/run"
    printf 'active-broker-v2\n' > "$ACTIVE"
    printf 'legacy-broker-v1\n' > "$LEGACY"
    printf 'foreign-owner\n' > "$FOREIGN"
    chmod 0700 "$ACTIVE" "$LEGACY" "$FOREIGN"
    : > "$SIGNALS"
    : > "$LAUNCHES"
    clear_net_unix "$PROC_ROOT"

    SIGNAL_HOOK="$CASE_DIR/signal-hook"
    cat > "$SIGNAL_HOOK" <<'EOF'
#!/bin/sh
printf '%s %s\n' "$1" "$2" >> "$FIXTURE_SIGNALS"
case "$FIXTURE_SIGNAL_MODE:$1" in
    socketless_registered:TERM)
        rm -rf "$FIXTURE_PROC/$2"
        clear_target="$FIXTURE_PROC/net/unix"
        cat > "$clear_target" <<'NET'
Num       RefCount Protocol Flags    Type St Inode Path
NET
        ;;
    socketless:TERM)
        rm -rf "$FIXTURE_PROC/$2"
        ;;
    stale:KILL)
        rm -rf "$FIXTURE_PROC/$2"
        cat > "$FIXTURE_PROC/net/unix" <<'NET'
Num       RefCount Protocol Flags    Type St Inode Path
NET
        ;;
    proc_loss:KILL)
        rm -rf "$FIXTURE_PROC/$2"
        rm -f "$FIXTURE_PROC/net/unix"
        ;;
    zombie:TERM)
        rm -f "$FIXTURE_PROC/$2/exe"
        awk '{$3="Z"; print}' "$FIXTURE_PROC/$2/stat" > "$FIXTURE_PROC/$2/stat.tmp"
        mv "$FIXTURE_PROC/$2/stat.tmp" "$FIXTURE_PROC/$2/stat"
        cat > "$FIXTURE_PROC/net/unix" <<'NET'
Num       RefCount Protocol Flags    Type St Inode Path
NET
        ;;
    reuse:TERM)
        rm -f "$FIXTURE_PROC/$2/exe"
        ln -s "$FIXTURE_FOREIGN" "$FIXTURE_PROC/$2/exe"
        printf '%s\0%s\0' "$FIXTURE_FOREIGN" "$FIXTURE_SOCKET" \
            > "$FIXTURE_PROC/$2/cmdline"
        ;;
esac
EOF
    chmod 0700 "$SIGNAL_HOOK"

    LAUNCH_HOOK="$CASE_DIR/launch-hook"
    cat > "$LAUNCH_HOOK" <<'EOF'
#!/bin/sh
printf '%s %s\n' "$1" "$2" >> "$FIXTURE_LAUNCHES"
printf '%s\n' "${ADVC_DECODE_PRIME_VALIDATION:-}" \
    > "${FIXTURE_LAUNCHES}.decode-prime-gate"
rm -f "$FIXTURE_SOCKET"
python3 - "$FIXTURE_SOCKET" <<'PY'
import socket
import sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.bind(sys.argv[1])
s.close()
PY
pid=900
inode=90090
mkdir -p "$FIXTURE_PROC/$pid/fd"
ln -s "$1" "$FIXTURE_PROC/$pid/exe"
printf 'Uid:\t0\t0\t0\t0\n' > "$FIXTURE_PROC/$pid/status"
printf '%s (advc-broker) S 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 90000 0\n' \
    "$pid" > "$FIXTURE_PROC/$pid/stat"
printf '%s\0%s\0' "$1" "$2" > "$FIXTURE_PROC/$pid/cmdline"
ln -s "socket:[$inode]" "$FIXTURE_PROC/$pid/fd/3"
cat > "$FIXTURE_PROC/net/unix" <<NET
Num       RefCount Protocol Flags    Type St Inode Path
00000000: 00000002 00000000 00010000 0001 01 $inode $2
NET
printf '%s\n' "$pid"
EOF
    chmod 0700 "$LAUNCH_HOOK"
}

run_service() {
    signal_mode=$1
    set +e
    env MODDIR="$MODULE_DIR" \
        ADVC_STATE_DIR="$STATE_DIR" \
        ADVC_PROC_ROOT="$PROC_ROOT" \
        ADVC_BROKER_PATH="$ACTIVE" \
        ADVC_PRODUCTION_SOCKET="$SOCKET" \
        ADVC_LEGACY_BROKER_PATH="$LEGACY" \
        ADVC_SIGNAL_HOOK="$SIGNAL_HOOK" \
        ADVC_LAUNCH_HOOK="$LAUNCH_HOOK" \
        ADVC_WAIT_SECONDS=0 \
        ADVC_TRUSTED_SOCKET_UID="$(id -u)" \
        ADVC_TRUSTED_SOCKET_GID="$(id -g)" \
        ADVC_TERM_WAIT_STEPS=2 \
        ADVC_KILL_WAIT_STEPS=2 \
        ADVC_START_WAIT_STEPS=2 \
        FIXTURE_SIGNAL_MODE="$signal_mode" \
        FIXTURE_SIGNALS="$SIGNALS" \
        FIXTURE_LAUNCHES="$LAUNCHES" \
        FIXTURE_PROC="$PROC_ROOT" \
        FIXTURE_FOREIGN="$FOREIGN" \
        FIXTURE_SOCKET="$SOCKET" \
        /bin/sh "$SERVICE"
    SERVICE_RC=$?
    set -e
}

setup_owner() {
    exe=$1
    make_socket_node "$SOCKET"
    write_net_unix "$PROC_ROOT" 70070 "$SOCKET"
    make_proc "$PROC_ROOT" 700 "$exe" "$SOCKET" 70070 70000
}

# An already-current exact owner is retained and its atomic PID state repaired.
setup_case same_hash
setup_owner "$ACTIVE"
run_service none
[ "$SERVICE_RC" -eq 0 ] || fail "same hash returned $SERVICE_RC"
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 700 ] || fail 'same hash PID state missing'
[ ! -s "$SIGNALS" ] || fail 'same hash sent a signal'
[ ! -s "$LAUNCHES" ] || fail 'same hash launched a replacement'

# Two simultaneous hooks serialize on the coordinator lock and leave one
# complete PID record, never a partially written temporary state file.
(run_service none; exit "$SERVICE_RC") &
same_hash_first=$!
(run_service none; exit "$SERVICE_RC") &
same_hash_second=$!
wait "$same_hash_first"
wait "$same_hash_second"
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 700 ] ||
    fail 'concurrent same-hash starts corrupted PID state'
if find "$STATE_DIR" -name 'advc-broker.pid.tmp.*' -print -quit | grep -q .; then
    fail 'concurrent starts left a partial PID state file'
fi
[ ! -s "$SIGNALS" ] || fail 'concurrent same-hash starts sent a signal'
[ ! -s "$LAUNCHES" ] || fail 'concurrent same-hash starts launched a replacement'

# A trusted stale owner is terminated once, its unchanged named socket is
# removed, and the active broker is launched and recorded.
setup_case stale_exact
setup_owner "$LEGACY"
run_service stale
[ "$SERVICE_RC" -eq 0 ] || fail "stale rollover returned $SERVICE_RC"
[ "$(printf '%s\n' 'TERM 700' 'KILL 700')" = "$(cat "$SIGNALS")" ] ||
    fail 'stale rollover bounded TERM/KILL signal set is not exact'
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 900 ] || fail 'replacement PID state missing'
[ "$(wc -l < "$LAUNCHES")" -eq 1 ] || fail 'replacement was not launched exactly once'
[ -S "$SOCKET" ] || fail 'replacement socket missing'
[ "$(cat "$LAUNCHES.decode-prime-gate")" = \
    validated-qcom-prime-repack-linear-fence-eos-v1 ] ||
    fail 'replacement broker did not inherit the exact decode PRIME gate'

# Mounting a fresh chroot /run can hide the exact production socket while the
# recorded broker remains alive. Retire only that identity-checked PID, then
# recreate the production socket in the current mount.
setup_case socketless_recorded
make_proc "$PROC_ROOT" 700 "$ACTIVE" "$SOCKET" 70070 70000
clear_net_unix "$PROC_ROOT"
printf '700\n' > "$STATE_DIR/advc-broker.pid"
run_service socketless
[ "$SERVICE_RC" -eq 0 ] || fail "socketless recorded owner returned $SERVICE_RC"
[ "$(cat "$SIGNALS")" = 'TERM 700' ] || fail 'socketless owner signal set is not exact'
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 900 ] || fail 'socketless replacement PID missing'
[ "$(wc -l < "$LAUNCHES")" -eq 1 ] || fail 'socketless owner replacement count is not one'
[ -S "$SOCKET" ] || fail 'socketless replacement socket missing'

# A newly mounted /run can also hide the filesystem node while procfs still
# registers the old listener path. Require the registry owner and recorded PID
# to be the same exact trusted process before retirement.
setup_case socketless_registered_owner
make_proc "$PROC_ROOT" 700 "$ACTIVE" "$SOCKET" 70070 70000
write_net_unix "$PROC_ROOT" 70070 "$SOCKET"
printf '700\n' > "$STATE_DIR/advc-broker.pid"
run_service socketless_registered
[ "$SERVICE_RC" -eq 0 ] || fail "registered socketless owner returned $SERVICE_RC"
[ "$(cat "$SIGNALS")" = 'TERM 700' ] || fail 'registered socketless signal set is not exact'
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 900 ] || fail 'registered socketless replacement PID missing'
[ "$(wc -l < "$LAUNCHES")" -eq 1 ] || fail 'registered socketless replacement count is not one'
[ -S "$SOCKET" ] || fail 'registered socketless replacement socket missing'

# A normally terminated process may remain as the same zombie while its
# executable link is already gone. Its unchanged start time proves that this
# is not PID reuse, so complete the exact rollover without sending KILL.
setup_case zombie_exit
setup_owner "$LEGACY"
run_service zombie
[ "$SERVICE_RC" -eq 0 ] || fail "zombie rollover returned $SERVICE_RC"
[ "$(cat "$SIGNALS")" = 'TERM 700' ] || fail 'zombie rollover signal set is not exact'
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 900 ] || fail 'zombie replacement PID missing'
[ "$(wc -l < "$LAUNCHES")" -eq 1 ] || fail 'zombie replacement not launched once'
[ -S "$SOCKET" ] || fail 'zombie replacement socket missing'

# If the PID changes identity after TERM, never send KILL, remove its socket, or
# launch a replacement under an unproven state.
setup_case pid_reuse
setup_owner "$LEGACY"
run_service reuse
[ "$SERVICE_RC" -ne 0 ] || fail 'PID reuse was accepted'
[ "$(cat "$SIGNALS")" = 'TERM 700' ] || fail 'PID reuse sent a non-exact signal'
! grep -q '^KILL ' "$SIGNALS" || fail 'PID reuse received KILL'
[ ! -s "$LAUNCHES" ] || fail 'PID reuse launched a replacement'
[ -S "$SOCKET" ] || fail 'PID reuse removed the socket'

# A socket whose owner executable is outside the explicit allowlist is never
# signalled or unlinked.
setup_case foreign_owner
setup_owner "$FOREIGN"
run_service none
[ "$SERVICE_RC" -ne 0 ] || fail 'foreign owner was accepted'
[ ! -s "$SIGNALS" ] || fail 'foreign owner was signalled'
[ ! -s "$LAUNCHES" ] || fail 'foreign owner launched a replacement'
[ -S "$SOCKET" ] || fail 'foreign owner socket was removed'

# A registered socket with no provable /proc owner fails closed.
setup_case missing_proc
make_socket_node "$SOCKET"
write_net_unix "$PROC_ROOT" 80080 "$SOCKET"
run_service none
[ "$SERVICE_RC" -ne 0 ] || fail 'missing proc owner was accepted'
[ ! -s "$SIGNALS" ] || fail 'missing proc owner was signalled'
[ ! -s "$LAUNCHES" ] || fail 'missing proc owner launched a replacement'
[ -S "$SOCKET" ] || fail 'missing proc socket was removed'

# A root-owned socket node that is provably absent from a readable registry is
# an unbound filesystem remnant, not an active foreign listener. Remove only
# that exact stable node and start the active broker.
setup_case unbound_stale_node
make_socket_node "$SOCKET"
clear_net_unix "$PROC_ROOT"
run_service none
[ "$SERVICE_RC" -eq 0 ] || fail "unbound stale node returned $SERVICE_RC"
[ ! -s "$SIGNALS" ] || fail 'unbound stale node sent a signal'
[ "$(wc -l < "$LAUNCHES")" -eq 1 ] ||
    fail 'unbound stale node did not launch exactly one replacement'
[ "$(cat "$STATE_DIR/advc-broker.pid")" = 900 ] ||
    fail 'unbound stale node replacement PID state missing'
[ -S "$SOCKET" ] || fail 'unbound stale node replacement socket missing'

# Even after an exact stale PID has exited, loss of the socket registry makes
# unlinking unprovable. Preserve the named node and refuse replacement.
setup_case proc_loss_after_kill
setup_owner "$LEGACY"
run_service proc_loss
[ "$SERVICE_RC" -ne 0 ] || fail 'post-exit proc loss was accepted'
[ "$(printf '%s\n' 'TERM 700' 'KILL 700')" = "$(cat "$SIGNALS")" ] ||
    fail 'post-exit proc loss signal set is not exact'
[ ! -s "$LAUNCHES" ] || fail 'post-exit proc loss launched a replacement'
[ -S "$SOCKET" ] || fail 'post-exit proc loss removed the socket'

# Release startup removes legacy diagnostics and never creates persistent
# broker/session logs. The dev flavor retains the bounded diagnostic channel.
setup_case release_no_logs
printf 'old broker log\n' > "$STATE_DIR/advc-broker.log"
printf 'old session log\n' > "$STATE_DIR/session.log"
run_service none
[ "$SERVICE_RC" -eq 0 ] || fail "release no-log startup returned $SERVICE_RC"
[ ! -e "$STATE_DIR/advc-broker.log" ] || fail 'release retained broker log'
[ ! -e "$STATE_DIR/session.log" ] || fail 'release retained session log'

setup_case dev_logs
printf 'BUILD_FLAVOR=dev\n' > "$MODULE_DIR/flavor.conf"
run_service none
[ "$SERVICE_RC" -eq 0 ] || fail "dev logging startup returned $SERVICE_RC"
[ -s "$STATE_DIR/session.log" ] || fail 'dev session log was not created'

if grep -Eq '(^|[^[:alnum:]_])(pkill|killall)([^[:alnum:]_]|$)' "$SERVICE"; then
    fail 'broker service contains a broad process kill'
fi

printf 'ADVC broker rollover fixture tests: PASS\n'
