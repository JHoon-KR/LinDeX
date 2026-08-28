#!/system/bin/sh
MODDIR=${0%/*}
STATE=/data/adb/debian-drm-lease-kit/state
KSU_BIN=/data/adb/ksu/bin

register_ksu_command() {
    name=$1
    source=$MODDIR/bin/$name
    target=$KSU_BIN/$name
    [ -d "$KSU_BIN" ] && [ -x "$source" ] || return 0

    if [ -L "$target" ]; then
        current=$(readlink "$target" 2>/dev/null)
        case "$current" in
            /data/adb/modules/debian_chroot/bin/$name|/data/adb/modules_update/debian_chroot/bin/$name)
                ln -sfn "$source" "$target"
                ;;
        esac
    elif [ ! -e "$target" ]; then
        ln -s "$source" "$target"
    fi
}

mkdir -p "$STATE"
# v3 deliberately creates no /system overlay. Expose only the optional codec
# tools through KernelSU's private PATH directory, never replacing a command
# owned by another module or user.
register_ksu_command advc-capability-probe
register_ksu_command advc-broker
# KernelSU may invoke late_start and boot-completed hooks close together.  A
# short-lived coordinator lock serializes retirement and replacement.  The
# watcher has its own lifetime flock, so a second coordinator can only replace
# the first watcher, never overlap it.
exec 9<>"$STATE/service-restart.lock"
# Android mksh marks auxiliary descriptors close-on-exec.  Duplicating the
# descriptor onto stdin for the flock subprocess locks the same open file
# description retained by this shell.
flock -x 0 <&9 || exit 0

# KernelSU's emulated soft reboot may leave a previous late-start shell alive.
# Retire every older watcher before starting the lock-protected current one.
old_pids=$(ps -A -o PID,ARGS 2>/dev/null | awk -v target="$MODDIR/bin/auto-service" '
    length($0) >= length(target) && substr($0, length($0) - length(target) + 1) == target { print $1 }
')
for pid in $old_pids; do
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    kill "$pid" 2>/dev/null || true
done
for attempt in 1 2 3 4 5; do
    alive=0
    for pid in $old_pids; do
        kill -0 "$pid" 2>/dev/null && alive=1
    done
    [ "$alive" = 0 ] && break
    sleep 1
done
for pid in $old_pids; do
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
done

"$MODDIR/bin/auto-service" &
watcher_pid=$!
watcher_ready=0
for attempt in 1 2 3 4 5; do
    recorded_pid=$(cat "$STATE/auto-service.pid" 2>/dev/null)
    if [ "$recorded_pid" = "$watcher_pid" ] && kill -0 "$watcher_pid" 2>/dev/null; then
        watcher_ready=1
        break
    fi
    kill -0 "$watcher_pid" 2>/dev/null || break
    sleep 1
done
flock -u 0 <&9
if [ "$watcher_ready" != 1 ]; then
    kill "$watcher_pid" 2>/dev/null || true
    wait "$watcher_pid" 2>/dev/null || true
    exit 1
fi
wait "$watcher_pid"
