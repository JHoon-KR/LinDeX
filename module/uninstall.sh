#!/system/bin/sh
MODDIR=${0%/*}
if [ -x "$MODDIR/bin/debian-gpu-control" ]; then
    "$MODDIR/bin/debian-gpu-control" stop >/dev/null 2>&1 || true
fi

for name in advc-capability-probe advc-broker; do
    target=/data/adb/ksu/bin/$name
    if [ -L "$target" ]; then
        linked=$(readlink "$target" 2>/dev/null)
        case "$linked" in
            /data/adb/modules/debian_chroot/bin/$name|/data/adb/modules_update/debian_chroot/bin/$name)
                rm -f "$target"
                ;;
        esac
    fi
done

ROOTFS=/data/local/debian
PERSIST=/data/adb/debian-drm-lease-kit
[ "$ROOTFS" = /data/local/debian ] || exit 1
[ "$PERSIST" = /data/adb/debian-drm-lease-kit ] || exit 1

echo "Removing Debian chroot mounts and all module state"
mkdir -p "$PERSIST"
mount_list="$PERSIST/uninstall-mounts.$$"
awk '$2 == "/data/local/debian" || index($2, "/data/local/debian/") == 1 { print length($2), $2 }' \
    /proc/mounts 2>/dev/null | sort -rn > "$mount_list"
while read -r _ mountpoint; do
    case "$mountpoint" in
        /data/local/debian|/data/local/debian/*)
            umount -l "$mountpoint" 2>/dev/null || \
                echo "Warning: initial unmount failed: $mountpoint" >&2
            ;;
    esac
done < "$mount_list"
rm -f "$mount_list"

remaining_mounts=$(awk '$2 == "/data/local/debian" || index($2, "/data/local/debian/") == 1 { print $2 }' \
    /proc/mounts 2>/dev/null)
if [ -n "$remaining_mounts" ]; then
    echo "Cannot remove Debian chroot: mounts are still attached:" >&2
    printf '%s\n' "$remaining_mounts" >&2
    echo "Rootfs and persistent state were preserved." >&2
    exit 1
fi

rm -rf "$ROOTFS"
if [ -e "$ROOTFS" ]; then
    echo "Cannot remove Debian chroot; persistent state was preserved." >&2
    exit 1
fi
if ! rm -rf "$PERSIST" || [ -e "$PERSIST" ]; then
    echo "Debian chroot was removed, but persistent module state could not be removed." >&2
    exit 1
fi
echo "Removed /data/local/debian and /data/adb/debian-drm-lease-kit"
