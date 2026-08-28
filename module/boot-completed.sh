#!/system/bin/sh
MODDIR=${0%/*}

# KernelSU emulated soft reboot runs boot-completed hooks. Restarting here
# guarantees that an updater's old watcher cannot retain stale shell functions.
nohup "$MODDIR/service.sh" >/dev/null 2>&1 &
