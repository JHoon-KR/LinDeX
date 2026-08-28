#!/system/bin/sh
# Shared control helpers for the v3 stock-compositor profile runtime.

resolve_moddir() {
    case "${MODDIR:-}" in
        '') MODDIR=${0%/*}; [ "${MODDIR##*/}" = bin ] && MODDIR=${MODDIR%/*} ;;
    esac
    CONFIG_DIR=${CONFIG_DIR:-/data/adb/debian-drm-lease-kit}
    CONFIG=$CONFIG_DIR/config.conf
    STATE=$CONFIG_DIR/state
    PIDFILE=$STATE/session.pid
    mkdir -p "$CONFIG_DIR" "$STATE"
    if [ ! -r "$CONFIG" ] && [ -r "$MODDIR/config.conf" ]; then
        cp "$MODDIR/config.conf" "$CONFIG"
        chmod 0600 "$CONFIG"
    fi
    BUILD_FLAVOR=release
    [ -r "$MODDIR/flavor.conf" ] && . "$MODDIR/flavor.conf"
    case "$BUILD_FLAVOR" in release|dev) ;; *) BUILD_FLAVOR=release ;; esac
    if [ "$BUILD_FLAVOR" = dev ]; then
        LOGFILE=$STATE/session.log
        SETUP_LOG=$STATE/setup.log
    else
        LOGFILE=/dev/null
        SETUP_LOG=/dev/null
        rm -f "$STATE/session.log" "$STATE/session.previous.log" \
            "$STATE/setup.log" "$STATE/setup.previous.log" \
            "$STATE/advc-broker.log" "$STATE/advc-broker.previous.log"
    fi
}

profile_valid() {
    case "$1" in
        lxqt|xfce|sway) return 0 ;;
        *) return 1 ;;
    esac
}

load_config() {
    AUTO_ATTACH=1
    PROFILE=sway
    SWAY_THEME=dark
    MESA_MODE=turnip
    DISPLAY_MODE=auto
    MODE_POLICY=preferred
    OUTPUT_MODIFIER_POLICY=ubwc
    DIRECT_SCANOUT=auto
    ALLOW_XWAYLAND=0
    SHARE_TOUCH=0
    USB_INPUT_MODE=linux-exclusive
    VIDEO_ACCELERATION=auto
    RESTORE_ANDROID=1
    ROOTFS=/data/local/debian
    SESSION_SECONDS=0
    [ -r "$CONFIG" ] && . "$CONFIG"
    profile_valid "$PROFILE" || PROFILE=sway
    case "$SWAY_THEME" in dark|light|pywal) ;; *) SWAY_THEME=dark ;; esac
    # v3 development builds used "standard" for the same standard archive
    # now exposed as the recommended patched-Turnip mode.  Canonicalize the
    # persisted value so an update does not leave a removed WebUI option
    # selected.
    case "$MESA_MODE" in
        standard) MESA_MODE=turnip ;;
        turnip|turnip-unpatched|system) ;;
        *) MESA_MODE=turnip ;;
    esac
    case "$AUTO_ATTACH" in 0|1) ;; *) AUTO_ATTACH=1 ;; esac
    case "$ALLOW_XWAYLAND" in 0|1) ;; *) ALLOW_XWAYLAND=0 ;; esac
    case "$SHARE_TOUCH" in 0|1) ;; *) SHARE_TOUCH=0 ;; esac
    case "$USB_INPUT_MODE" in shared|linux-exclusive) ;; *) USB_INPUT_MODE=linux-exclusive ;; esac
    case "$VIDEO_ACCELERATION" in auto|disabled) ;; *) VIDEO_ACCELERATION=auto ;; esac
    case "$RESTORE_ANDROID" in 0|1) ;; *) RESTORE_ANDROID=1 ;; esac
    case "$OUTPUT_MODIFIER_POLICY" in auto|linear|ubwc) ;; *) OUTPUT_MODIFIER_POLICY=ubwc ;; esac
    case "$DIRECT_SCANOUT" in auto|off) ;; *) DIRECT_SCANOUT=auto ;; esac
}

write_state_value() {
    state_file=$1
    state_value=$2
    state_tmp=$state_file.tmp.$$
    printf '%s\n' "$state_value" > "$state_tmp" || return 1
    chmod 0600 "$state_tmp" 2>/dev/null || true
    mv -f "$state_tmp" "$state_file"
}

log_event() {
    [ "$BUILD_FLAVOR" = dev ] || return 0
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOGFILE"
}

rotate_log_if_large() {
    [ "$BUILD_FLAVOR" = dev ] || return 0
    file=$1
    previous=$2
    [ -r "$file" ] || return 0
    bytes=$(wc -c < "$file" 2>/dev/null | tr -d ' ')
    case "$bytes" in ''|*[!0-9]*) return 0 ;; esac
    [ "$bytes" -gt 524288 ] || return 0
    tail -c 262144 "$file" > "$previous.tmp.$$" 2>/dev/null || return 0
    mv -f "$previous.tmp.$$" "$previous"
    : > "$file"
}

maintain_logs() {
    [ "$BUILD_FLAVOR" = dev ] || return 0
    rotate_log_if_large "$LOGFILE" "$STATE/session.previous.log"
    rotate_log_if_large "$SETUP_LOG" "$STATE/setup.previous.log"
    rotate_log_if_large "$STATE/advc-broker.log" "$STATE/advc-broker.previous.log"
}

hardware_check() {
    [ -c /dev/kgsl-3d0 ] || [ -d /sys/class/kgsl/kgsl-3d0 ] || return 10
    [ -c /dev/dri/card0 ] || return 11
    [ -d "$ROOTFS" ] && [ ! -L "$ROOTFS" ] || return 12
}

ensure_mountpoint() {
    advc_mount_target=$1
    shift
    mountpoint -q "$advc_mount_target" && return 0
    "$@" && return 0
    # A concurrent WebUI/manual start may have mounted it after our check.
    mountpoint -q "$advc_mount_target"
}

ensure_chroot_mounts() {
    [ "$(id -u)" -eq 0 ] || {
        echo 'LinDeX chroot initialization requires root' >&2
        return 77
    }
    [ -d "$ROOTFS" ] && [ ! -L "$ROOTFS" ] || {
        echo "LinDeX chroot is unavailable: $ROOTFS" >&2
        return 78
    }

    # Child directories are created after their parent mount becomes visible.
    # Creating them earlier makes /dev hide pts/shm and /run hide its state.
    mkdir -p "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" "$ROOTFS/run" \
        "$ROOTFS/tmp" || return 78
    ensure_mountpoint "$ROOTFS/dev" \
        mount -o bind /dev "$ROOTFS/dev" || return 78
    mkdir -p "$ROOTFS/dev/pts" "$ROOTFS/dev/shm" || return 78
    ensure_mountpoint "$ROOTFS/dev/pts" \
        mount -t devpts devpts "$ROOTFS/dev/pts" || return 78
    ensure_mountpoint "$ROOTFS/dev/shm" \
        mount -t tmpfs -o rw,nosuid,nodev,mode=1777 tmpfs \
            "$ROOTFS/dev/shm" || return 78
    ensure_mountpoint "$ROOTFS/proc" \
        mount -t proc proc "$ROOTFS/proc" || return 78
    ensure_mountpoint "$ROOTFS/sys" \
        mount -t sysfs sys "$ROOTFS/sys" || return 78
    ensure_mountpoint "$ROOTFS/run" \
        mount -t tmpfs -o rw,nosuid,nodev,mode=0755 tmpfs \
            "$ROOTFS/run" || return 78

    mkdir -p "$ROOTFS/run/user/0" "$ROOTFS/run/lock" \
        "$ROOTFS/run/android-drm" || return 78
    chmod 0700 "$ROOTFS/run/user/0" "$ROOTFS/run/android-drm" || return 78
    chmod 0775 "$ROOTFS/run/lock" || return 78
    chmod 1777 "$ROOTFS/tmp" || return 78

    # libdrm/libva resolve DRM fds through procfs. Fail closed on a partial
    # initialization instead of silently hiding the hardware codec profiles.
    mountpoint -q "$ROOTFS/dev" &&
        mountpoint -q "$ROOTFS/dev/pts" &&
        mountpoint -q "$ROOTFS/dev/shm" &&
        mountpoint -q "$ROOTFS/proc" &&
        mountpoint -q "$ROOTFS/sys" &&
        mountpoint -q "$ROOTFS/run" &&
        [ -d "$ROOTFS/dev/dri" ] &&
        [ -e "$ROOTFS/proc/self/fd" ] &&
        [ -d "$ROOTFS/sys/class/drm" ] || {
            echo 'LinDeX chroot initialization is incomplete' >&2
            return 78
        }
}

# The VA-API driver lives inside the Debian rootfs while its Android codec
# broker runs outside the chroot.  Keep both paths canonical so every caller
# validates the same immutable driver and the same production socket.
ADVC_CODEC_ROOT_GUEST=/opt/android-drm-lease-kit/codec
ADVC_VAAPI_DRIVER_DIR_GUEST=$ADVC_CODEC_ROOT_GUEST
ADVC_VAAPI_DRIVER_GUEST=$ADVC_VAAPI_DRIVER_DIR_GUEST/advc_drv_video.so
ADVC_VAAPI_MANIFEST_GUEST=$ADVC_VAAPI_DRIVER_DIR_GUEST/SHA256SUMS
ADVC_REPACK_GATEWAY_GUEST=$ADVC_CODEC_ROOT_GUEST/advc-repack-gateway
ADVC_FIREFOX_RDD_SOCKET_GUEST=$ADVC_CODEC_ROOT_GUEST/liblindex-firefox-advc-rdd-socket.so
ADVC_FIREFOX_EGL_IDENTITY_GUEST=$ADVC_CODEC_ROOT_GUEST/liblindex-firefox-egl-drm-identity.so
ADVC_VAAPI_SOCKET_GUEST=/run/android-drm/advc-broker-1.1.sock

advc_codec_driver_installed() {
    driver_root=$ROOTFS$ADVC_VAAPI_DRIVER_DIR_GUEST
    driver_file=$ROOTFS$ADVC_VAAPI_DRIVER_GUEST
    driver_manifest=$ROOTFS$ADVC_VAAPI_MANIFEST_GUEST
    [ -f "$driver_file" ] && [ ! -L "$driver_file" ] || return 1
    [ -f "$driver_manifest" ] && [ ! -L "$driver_manifest" ] || return 1
    [ -x "$ROOTFS$ADVC_REPACK_GATEWAY_GUEST" ] &&
        [ ! -L "$ROOTFS$ADVC_REPACK_GATEWAY_GUEST" ] || return 1
    [ -f "$ROOTFS$ADVC_FIREFOX_RDD_SOCKET_GUEST" ] &&
        [ ! -L "$ROOTFS$ADVC_FIREFOX_RDD_SOCKET_GUEST" ] || return 1
    [ -f "$ROOTFS$ADVC_FIREFOX_EGL_IDENTITY_GUEST" ] &&
        [ ! -L "$ROOTFS$ADVC_FIREFOX_EGL_IDENTITY_GUEST" ] || return 1
    (cd "$driver_root" && sha256sum -c SHA256SUMS >/dev/null 2>&1)
}

advc_codec_runtime_ready() {
    advc_codec_driver_installed || return 1
    [ -c "$ROOTFS/dev/dri/renderD128" ] || return 1
    [ -e "$ROOTFS/proc/self/fd" ] || return 1
    [ -d "$ROOTFS/sys/class/drm/renderD128" ] || return 1
    [ -S "$ROOTFS$ADVC_VAAPI_SOCKET_GUEST" ] || return 1
}

prepare_advc_codec_runtime() {
    [ "${VIDEO_ACCELERATION:-disabled}" = auto ] || return 1
    [ -x "$MODDIR/bin/advc-broker-service" ] || return 1
    [ -x "$MODDIR/bin/advc-broker" ] || return 1
    ensure_chroot_mounts || return 1
    advc_codec_driver_installed || return 1
    "$MODDIR/bin/advc-broker-service" >/dev/null 2>&1 || return 1
    advc_codec_runtime_ready
}

dp_connector_path() {
    drm_class=${DRM_CLASS:-/sys/class/drm}
    for connector in "$drm_class"/card*-DP-*; do
        [ -d "$connector" ] || continue
        [ "$(cat "$connector/status" 2>/dev/null)" = connected ] && {
            printf '%s\n' "$connector"
            return 0
        }
    done
    for connector in "$drm_class"/card*-DP-*; do
        [ -d "$connector" ] || continue
        printf '%s\n' "$connector"
        return 0
    done
    return 1
}

physical_dp_path() {
    drm_class=${DRM_CLASS:-/sys/class/drm}
    for connector in "$drm_class"/card*-DP-*; do
        [ -d "$connector" ] || continue
        [ "$(cat "$connector/status" 2>/dev/null)" = connected ] || continue
        # sysfs attributes commonly report st_size=0 even when reads return a
        # complete EDID. Validate the bytes read instead of file metadata.
        edid_bytes=$(wc -c < "$connector/edid" 2>/dev/null | tr -d ' ')
        case "$edid_bytes" in ''|*[!0-9]*) continue ;; esac
        [ "$edid_bytes" -ge 128 ] || continue
        printf '%s\n' "$connector"
        return 0
    done
    return 1
}

connected_dp_path() {
    drm_class=${DRM_CLASS:-/sys/class/drm}
    for connector in "$drm_class"/card*-DP-*; do
        [ -d "$connector" ] || continue
        [ "$(cat "$connector/status" 2>/dev/null)" = connected ] || continue
        printf '%s\n' "$connector"
        return 0
    done
    return 1
}

typec_partner_present() {
    typec_class=${TYPEC_CLASS:-/sys/class/typec}
    for partner in "$typec_class"/port*-partner; do
        [ -e "$partner" ] && return 0
    done
    return 1
}

valid_pid() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    [ "$1" -gt 1 ]
}

# Never let toybox tr hold a procfs cmdline/environ fd directly. If the target
# exits while that fd is open, affected Android kernels can leave tr spinning
# forever after its shell has gone away. A bounded dd owns the procfs fd and tr
# only consumes an ordinary pipe, so both sides have a finite lifetime.
read_proc_cmdline() {
    proc_pid=$1
    valid_pid "$proc_pid" || return 1
    [ -r "/proc/$proc_pid/cmdline" ] || return 1
    dd if="/proc/$proc_pid/cmdline" bs=4096 count=16 2>/dev/null |
        tr '\000' ' '
}

proc_environ_has_exact() {
    proc_pid=$1
    proc_expected=$2
    valid_pid "$proc_pid" || return 1
    [ -r "/proc/$proc_pid/environ" ] || return 1
    dd if="/proc/$proc_pid/environ" bs=4096 count=64 2>/dev/null |
        tr '\000' '\n' | grep -Fxq "$proc_expected"
}

# Check the complete process group instead of only its leader.  The launcher
# can exit before a compositor child closes its DRM/lease fd.  A per-session
# environment token makes cleanup fail closed if a stale PGID is ever reused.
owned_process_group_running() {
    owned_group=$1
    owned_token=${2:-}
    valid_pid "$owned_group" || return 1
    for owned_stat in /proc/[0-9]*/stat; do
        [ -r "$owned_stat" ] || continue
        owned_tail=$(sed 's/^.*) //' "$owned_stat" 2>/dev/null) || continue
        set -- $owned_tail
        [ "$#" -ge 3 ] || continue
        [ "$1" != Z ] || continue
        [ "$3" = "$owned_group" ] || continue
        [ -n "$owned_token" ] || return 0
        owned_pid=${owned_stat#/proc/}
        owned_pid=${owned_pid%/stat}
        proc_environ_has_exact "$owned_pid" \
            "LINDEX_SESSION_TOKEN=$owned_token" && return 0
    done
    return 1
}

session_start_intent_active() {
    requested_now=${1:-}
    intent_file=$STATE/session-start.intent
    [ -f "$intent_file" ] && [ ! -L "$intent_file" ] || return 1
    intent_line=$(cat "$intent_file" 2>/dev/null) || return 1
    set -- $intent_line
    [ "$#" -eq 2 ] || return 1
    intent_pid=$1
    intent_started=$2
    valid_pid "$intent_pid" || return 1
    case "$intent_started" in ''|*[!0-9]*) return 1 ;; esac

    intent_now=$requested_now
    case "$intent_now" in ''|*[!0-9]*) intent_now=$(date +%s) ;; esac
    case "$intent_now" in ''|*[!0-9]*) return 1 ;; esac
    [ "$intent_now" -ge "$intent_started" ] || return 1
    [ $((intent_now - intent_started)) -le 15 ] || return 1

    kill -0 "$intent_pid" 2>/dev/null || return 1
    [ "$(awk '{print $3}' "/proc/$intent_pid/stat" 2>/dev/null)" != Z ] || return 1
    intent_cmd=$(read_proc_cmdline "$intent_pid") || return 1
    case "$intent_cmd" in
        *"$MODDIR/bin/session-runner"*"$MODDIR/bin/launch-stock-profile"*) ;;
        *) return 1 ;;
    esac
    intent_state=$(cat "$STATE/session-state" 2>/dev/null)
    case "$intent_state" in starting|running) ;; *) return 1 ;; esac
    recorded_pid=$(cat "$PIDFILE" 2>/dev/null)
    if valid_pid "$recorded_pid" && [ "$recorded_pid" != "$intent_pid" ]; then
        return 1
    fi
    return 0
}

clear_session_start_intent() {
    expected_pid=${1:-}
    valid_pid "$expected_pid" || return 1
    intent_file=$STATE/session-start.intent
    [ -e "$intent_file" ] || return 0
    [ -f "$intent_file" ] && [ ! -L "$intent_file" ] || return 1
    intent_line=$(cat "$intent_file" 2>/dev/null) || return 1
    set -- $intent_line
    [ "$#" -eq 2 ] && [ "$1" = "$expected_pid" ] || return 1
    rm -f "$intent_file"
}

session_running() {
    session_pid=$(cat "$PIDFILE" 2>/dev/null)
    valid_pid "$session_pid" || return 1
    kill -0 "$session_pid" 2>/dev/null || { rm -f "$PIDFILE"; return 1; }
    [ "$(awk '{print $3}' "/proc/$session_pid/stat" 2>/dev/null)" != Z ] || {
        rm -f "$PIDFILE"
        return 1
    }
    cmdline=$(read_proc_cmdline "$session_pid") || {
        rm -f "$PIDFILE"
        return 1
    }
    case "$cmdline" in
        *"$MODDIR/bin/session-runner"*"$MODDIR/bin/launch-stock-profile"*) return 0 ;;
        *) rm -f "$PIDFILE"; return 1 ;;
    esac
}

recorded_session_group_running() {
    group_record=$(cat "$STATE/session.pgid" 2>/dev/null) || return 1
    set -- $group_record
    case "$#" in 1|2) ;; *) rm -f "$STATE/session.pgid"; return 1 ;; esac
    group_pid=$1
    group_token=${2:-}
    valid_pid "$group_pid" || return 1
    if [ -n "$group_token" ]; then
        owned_process_group_running "$group_pid" "$group_token" || {
            rm -f "$STATE/session.pgid"
            return 1
        }
        return 0
    fi

    # Compatibility with old one-field markers: only accept them while the
    # original launcher leader and its exact command identity still exist.
    kill -0 "$group_pid" 2>/dev/null || { rm -f "$STATE/session.pgid"; return 1; }
    [ "$(awk '{print $3}' "/proc/$group_pid/stat" 2>/dev/null)" != Z ] || {
        rm -f "$STATE/session.pgid"
        return 1
    }
    group_id=$(awk '{print $5}' "/proc/$group_pid/stat" 2>/dev/null)
    [ "$group_id" = "$group_pid" ] || return 1
    group_cmd=$(read_proc_cmdline "$group_pid") || return 1
    case "$group_cmd" in *"$MODDIR/bin/launch-stock-profile"*) return 0 ;; esac
    return 1
}

stop_recorded_session_group() {
    recorded_session_group_running || return 0
    group_record=$(cat "$STATE/session.pgid")
    set -- $group_record
    group_pid=$1
    group_token=${2:-}
    kill -TERM -- "-$group_pid" 2>/dev/null || kill -TERM "$group_pid" 2>/dev/null || true
    group_attempt=0
    group_attempt_limit=${SESSION_GROUP_TERM_TIMEOUT:-8}
    while owned_process_group_running "$group_pid" "$group_token" &&
          [ "$group_attempt" -lt "$group_attempt_limit" ]; do
        sleep 1
        group_attempt=$((group_attempt + 1))
    done
    owned_process_group_running "$group_pid" "$group_token" && {
        kill -KILL -- "-$group_pid" 2>/dev/null || kill -KILL "$group_pid" 2>/dev/null || true
    }
    rm -f "$STATE/session.pgid"
}

profile_installed() {
    profile_valid "$1" || return 1
    marker=$ROOTFS/var/lib/android-drm-lease-kit/profiles/$1.ready
    [ -r "$marker" ] || return 1
    [ "$(sed -n 's/^profile=//p' "$marker" 2>/dev/null | sed -n '1p')" = "$1" ] &&
    [ "$(sed -n 's/^state=//p' "$marker" 2>/dev/null | sed -n '1p')" = ready ] || return 1
    requirements=$(sed -n 's/^requirements=//p' "$marker" 2>/dev/null | sed -n '1p')
    case "$1" in
        sway)
            [ "$requirements" = package-command-and-appearance-verified ] &&
            [ "$(sed -n 's/^appearance_source_commit=//p' "$marker" 2>/dev/null | sed -n '1p')" = \
                e4d0126d7f236fee50a84fbb0e61498dcf5705e7 ] &&
            [ "$(sed -n 's/^appearance_archive_sha256=//p' "$marker" 2>/dev/null | sed -n '1p')" = \
                4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c ] &&
            [ "$(sed -n 's/^sway_theme=//p' "$marker" 2>/dev/null | sed -n '1p')" = "$SWAY_THEME" ]
            ;;
        *) [ "$requirements" = package-and-command-verified ] ;;
    esac
}

bridge_runtime_ready() {
    runtime=$ROOTFS/opt/android-drm-lease-kit/bridge/stock-profile-bridge-v12
    [ -r "$runtime/BRIDGE_RUNTIME.sha256" ] || return 1
    [ -x "$runtime/bin/drm_lease_client" ] || return 1
    [ -r "$runtime/lib/libandroid-drm-bridge.so.1" ] || return 1
    [ -r "$runtime/lib/libdrm_lease_seat.so" ] || return 1
    (cd "$runtime" && sha256sum -c BRIDGE_RUNTIME.sha256 >/dev/null 2>&1)
}

start_desktop() {
    if session_running; then
        write_state_value "$STATE/start-prepare-state" complete
        write_state_value "$STATE/start-prepare-detail" session-started
        rm -f "$STATE/last-error"
        return 0
    fi
    hardware_check || return $?
    physical_dp_path >/dev/null 2>&1 || return 20
    profile_installed "$PROFILE" || return 24
    bridge_runtime_ready || return 25
    rm -f "$STATE/manual_stop"
    write_state_value "$STATE/session-profile" "$PROFILE"
    write_state_value "$STATE/session-state" starting
    maintain_logs
    if [ "$BUILD_FLAVOR" = dev ]; then
        RESTORE_ANDROID_POLICY="$RESTORE_ANDROID" DISPLAY_MODE="$DISPLAY_MODE" \
            "$MODDIR/bin/session-runner" "$MODDIR/bin/launch-stock-profile" \
            --profile "$PROFILE" >> "$LOGFILE" 2>&1 &
    else
        RESTORE_ANDROID_POLICY="$RESTORE_ANDROID" DISPLAY_MODE="$DISPLAY_MODE" \
            "$MODDIR/bin/session-runner" "$MODDIR/bin/launch-stock-profile" \
            --profile "$PROFILE" >/dev/null 2>&1 &
    fi
    session_pid=$!
    printf '%s\n' "$session_pid" > "$PIDFILE"
    chmod 0600 "$PIDFILE"
    sleep 1
    if ! kill -0 "$session_pid" 2>/dev/null; then
        wait "$session_pid" 2>/dev/null || true
        rm -f "$PIDFILE"
        write_state_value "$STATE/session-state" failed
        return 30
    fi
    write_state_value "$STATE/session-state" running
    write_state_value "$STATE/start-prepare-state" complete
    write_state_value "$STATE/start-prepare-detail" session-started
    rm -f "$STATE/last-error"
    log_event "SESSION started profile=$PROFILE pid=$session_pid flavor=$BUILD_FLAVOR"
}

stop_desktop() {
    session_pid=$(cat "$PIDFILE" 2>/dev/null)
    if ! valid_pid "$session_pid" || ! kill -0 "$session_pid" 2>/dev/null; then
        rm -f "$PIDFILE"
        stop_recorded_session_group
        rm -f "$STATE/session-start.intent"
        write_state_value "$STATE/session-state" stopped
        return 0
    fi
    cmdline=$(read_proc_cmdline "$session_pid") || {
        echo "refusing to signal an unreadable PID: $session_pid" >&2
        return 31
    }
    case "$cmdline" in
        *"$MODDIR/bin/session-runner"*"$MODDIR/bin/launch-stock-profile"*) ;;
        *) echo "refusing to signal an unowned PID: $session_pid" >&2; return 31 ;;
    esac
    kill -TERM "$session_pid" 2>/dev/null || true
    attempt=0
    # session-runner can spend up to eight seconds stopping its owned process
    # group and then roughly twelve seconds proving the exact Android display
    # is stably ON.  Do not KILL it in the middle of that recovery window.
    while session_running && [ "$attempt" -lt 30 ]; do
        sleep 1
        attempt=$((attempt + 1))
    done
    session_running && kill -KILL "$session_pid" 2>/dev/null || true
    stop_recorded_session_group
    rm -f "$PIDFILE"
    rm -f "$STATE/session-start.intent"
    write_state_value "$STATE/session-state" stopped
    log_event "SESSION stopped profile=$PROFILE pid=$session_pid"
}
