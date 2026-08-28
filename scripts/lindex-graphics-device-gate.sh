#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Source-only, development-device graphics acceptance gate for LinDeX.
# This file is intentionally not installed by the Magisk/KernelSU module.
# It never toggles Type-C, DisplayManager state, or a physical DP link.

set -u
umask 077

CONTROL=${LINDEX_CONTROL:-/data/adb/modules/debian_chroot/bin/debian-gpu-control}
STATE=${LINDEX_STATE_DIR:-/data/adb/debian-drm-lease-kit/state}
GATE_ROOT=${LINDEX_GATE_ROOT:-/data/local/tmp/lindex-graphics-device-gate}
PROC_ROOT=${LINDEX_PROC_ROOT:-/proc}
CMD_TOOL=${LINDEX_CMD_TOOL:-/system/bin/cmd}
DUMPSYS_TOOL=${LINDEX_DUMPSYS_TOOL:-/system/bin/dumpsys}
CHROOT_TOOL=${LINDEX_CHROOT_TOOL:-/system/bin/chroot}
SLEEP_TOOL=${LINDEX_SLEEP_TOOL:-sleep}
START_WAIT_SECONDS=${LINDEX_START_WAIT_SECONDS:-300}
STOP_WAIT_SECONDS=${LINDEX_STOP_WAIT_SECONDS:-45}
POLL_SECONDS=${LINDEX_POLL_SECONDS:-2}
GETFB2_GUEST=${LINDEX_GETFB2_GUEST:-/tmp/lindex-graphics-gate/drm_lease_getfb2}

RESULT_PASS=0
RESULT_FAIL=1
RESULT_INDETERMINATE=2
RESULT_USAGE=64

usage() {
    cat <<'EOF'
usage: lindex-graphics-device-gate.sh PHASE PROFILE

Profiles: lxqt | xfce | sway

Phases:
  start             stop any owned session, select the profile, start it, and
                    collect the exact running identity and renderer evidence
  status            collect a non-mutating live status snapshot
  stop              stop the owned session and verify exact process/display
                    restoration evidence
  before-unplug     enable auto-attach and save exact PID/starttime identities
  after-unplug      after the operator physically disconnects DP, verify that
                    the old process identities and all owner markers are gone
  after-reconnect   after the operator reconnects DP, verify a fresh lease
                    session identity and DP/EDID readiness
  direct-start      Sway only: launch ordinary fullscreen vkmark and the
                    GETFB2 observer as Sway children on the inherited lease
  direct-check      Sway only: classify the completed direct-scanout evidence

Exit status: 0=PASS, 1=FAIL, 2=INDETERMINATE, 64=usage error.

The unplug/reconnect phases never manipulate the cable, Type-C state, kernel
connector, or Android display state. Run them only after the named physical
action has been performed by the operator.
EOF
}

valid_profile() {
    case "$1" in lxqt|xfce|sway) return 0 ;; *) return 1 ;; esac
}

valid_decimal() {
    case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}

safe_runtime_settings() {
    valid_decimal "$START_WAIT_SECONDS" && valid_decimal "$STOP_WAIT_SECONDS" &&
        valid_decimal "$POLL_SECONDS" && [ "$POLL_SECONDS" -gt 0 ]
}

json_string() {
    json_key=$1
    printf '%s\n' "$STATUS_JSON" |
        sed -n "s/.*\"$json_key\":\"\([^\"]*\)\".*/\1/p"
}

json_scalar() {
    json_key=$1
    printf '%s\n' "$STATUS_JSON" |
        sed -n "s/.*\"$json_key\":\([^,}]*\).*/\1/p"
}

read_key() {
    read_file=$1
    read_name=$2
    sed -n "s/^$read_name=//p" "$read_file" 2>/dev/null | sed -n '1p'
}

write_atomic() {
    write_path=$1
    write_value=$2
    write_tmp=$write_path.tmp.$$
    printf '%s\n' "$write_value" > "$write_tmp" || return 1
    chmod 0600 "$write_tmp" 2>/dev/null || true
    mv -f "$write_tmp" "$write_path"
}

process_starttime() {
    process_id=$1
    valid_decimal "$process_id" && [ "$process_id" -gt 1 ] || return 1
    [ -r "$PROC_ROOT/$process_id/stat" ] || return 1
    awk '{ print $22 }' "$PROC_ROOT/$process_id/stat" 2>/dev/null |
        sed -n '/^[0-9][0-9]*$/p' | sed -n '1p'
}

process_identity() {
    identity_id=$1
    identity_start=$(process_starttime "$identity_id") || return 1
    printf '%s:%s\n' "$identity_id" "$identity_start"
}

session_log_lines() {
    if [ -r "$STATE/session.log" ]; then
        line_count=$(wc -l < "$STATE/session.log" 2>/dev/null | tr -d ' ')
        valid_decimal "$line_count" && { printf '%s\n' "$line_count"; return; }
    fi
    printf '0\n'
}

same_identity_alive() {
    identity_value=$1
    case "$identity_value" in
        *:*) ;;
        *) return 1 ;;
    esac
    identity_id=${identity_value%%:*}
    identity_expected=${identity_value#*:}
    identity_current=$(process_starttime "$identity_id") || return 1
    [ "$identity_current" = "$identity_expected" ]
}

marker_value() {
    marker_name=$1
    if [ -f "$STATE/$marker_name" ] && [ ! -L "$STATE/$marker_name" ]; then
        sed -n '1p' "$STATE/$marker_name" 2>/dev/null
    fi
}

session_group_pid() {
    group_record=$(marker_value session.pgid)
    set -- $group_record
    case "$#" in 1|2) ;; *) return 1 ;; esac
    valid_decimal "$1" || return 1
    printf '%s\n' "$1"
}

session_group_token() {
    group_record=$(marker_value session.pgid)
    set -- $group_record
    [ "$#" -eq 2 ] || return 1
    case "$2" in lindex-*) printf '%s\n' "$2" ;; *) return 1 ;; esac
}

external_display_line() {
    display_id=$1
    valid_decimal "$display_id" || return 1
    "$CMD_TOOL" display get-displays --type external 2>/dev/null |
        awk -v id="$display_id" '
            index($0, "Display id " id ":") == 1 { print; exit }
        ' | grep . && return 0
    "$DUMPSYS_TOOL" display 2>/dev/null |
        awk -v id="$display_id" '
            /DisplayInfo\{/ &&
            ($0 ~ ("displayId[ =]" id "([, }]|$)")) &&
            ($0 ~ /type[ =]EXTERNAL([, }]|$)/) { print; exit }
        ' | grep .
}

external_display_state() {
    display_id=$1
    if ! display_line=$(external_display_line "$display_id"); then
        printf 'DISCONNECTED\n'
        return 1
    fi
    case "$display_line" in
        *" state ON,"*|*" state=ON,"*|*" state ON}"*|*" state=ON}")
            printf 'ON\n'
            ;;
        *" state OFF,"*|*" state=OFF,"*|*" state OFF}"*|*" state=OFF}")
            printf 'OFF\n'
            ;;
        *)
            printf 'UNKNOWN\n'
            return 1
            ;;
    esac
}

load_status() {
    if STATUS_JSON=$($CONTROL status-json 2>&1); then
        STATUS_RC=0
    else
        STATUS_RC=$?
    fi
    printf '%s\n' "$STATUS_JSON" > "$EVIDENCE/$PHASE-status.json"
    chmod 0600 "$EVIDENCE/$PHASE-status.json" 2>/dev/null || true
    [ "$STATUS_RC" -eq 0 ]
}

write_snapshot() {
    snapshot_path=$EVIDENCE/$PHASE-snapshot.txt
    snapshot_tmp=$snapshot_path.tmp.$$
    session_pid=$(marker_value session.pid)
    session_pgid=$(session_group_pid)
    session_group_token_value=$(session_group_token 2>/dev/null || true)
    display_id=$(marker_value last_dp_display_id)
    session_identity=$(process_identity "$session_pid" 2>/dev/null || true)
    group_identity=$(process_identity "$session_pgid" 2>/dev/null || true)
    display_state=NO_CAPTURE
    display_line=NO_CAPTURE
    if valid_decimal "$display_id"; then
        display_state=$(external_display_state "$display_id" 2>/dev/null || true)
        [ -n "$display_state" ] || display_state=UNKNOWN
        display_line=$(external_display_line "$display_id" 2>/dev/null || true)
        [ -n "$display_line" ] || display_line=UNAVAILABLE
    fi
    {
        printf 'phase=%s\nprofile=%s\n' "$PHASE" "$PROFILE"
        printf 'status_rc=%s\n' "$STATUS_RC"
        printf 'dp=%s\nedid=%s\nrunning=%s\n' \
            "$(json_string dp)" "$(json_string edidState)" \
            "$(json_scalar running)"
        printf 'reported_pid=%s\nsession_identity=%s\ngroup_identity=%s\n' \
            "$(json_scalar pid)" "${session_identity:-missing}" \
            "${group_identity:-missing}"
        printf 'group_token=%s\n' "${session_group_token_value:-missing}"
        printf 'session_state=%s\n' "$(marker_value session-state)"
        for marker in session.pid session.pgid session-start.intent; do
            if [ -e "$STATE/$marker" ]; then
                printf 'marker_%s=present:%s\n' \
                    "$(printf '%s' "$marker" | tr '.-' '__')" \
                    "$(marker_value "$marker")"
            else
                printf 'marker_%s=absent\n' \
                    "$(printf '%s' "$marker" | tr '.-' '__')"
            fi
        done
        printf 'external_display_id=%s\nexternal_display_state=%s\n' \
            "${display_id:-missing}" "$display_state"
        printf 'external_display_line=%s\n' "$display_line"
    } > "$snapshot_tmp" || return 1
    chmod 0600 "$snapshot_tmp" 2>/dev/null || true
    mv -f "$snapshot_tmp" "$snapshot_path"
}

record_result() {
    result=$1
    reason=$2
    result_path=$EVIDENCE/$PHASE-result.txt
    result_tmp=$result_path.tmp.$$
    {
        printf 'LINDEX_GRAPHICS_GATE schema=1 phase=%s profile=%s result=%s reason=%s\n' \
            "$PHASE" "$PROFILE" "$result" "$reason"
        printf 'evidence=%s\n' "$EVIDENCE"
        if [ "$PHASE" = after-reconnect ]; then
            printf 'manual_visual_confirmation=required\n'
        fi
    } > "$result_tmp" || exit "$RESULT_FAIL"
    chmod 0600 "$result_tmp" 2>/dev/null || true
    mv -f "$result_tmp" "$result_path"
    cat "$result_path"
    case "$result" in
        PASS) exit "$RESULT_PASS" ;;
        FAIL) exit "$RESULT_FAIL" ;;
        INDETERMINATE) exit "$RESULT_INDETERMINATE" ;;
        *) exit "$RESULT_FAIL" ;;
    esac
}

running_machine_check() {
    CHECK_RESULT=PASS
    CHECK_REASON=running-machine-evidence-complete
    [ "$STATUS_RC" -eq 0 ] || {
        CHECK_RESULT=FAIL; CHECK_REASON=status-command-failed; return
    }
    [ "$(json_string profile)" = "$PROFILE" ] || {
        CHECK_RESULT=FAIL; CHECK_REASON=wrong-active-profile; return
    }
    [ "$(json_string dp)" = connected ] &&
        [ "$(json_string edidState)" = ready ] || {
        CHECK_RESULT=INDETERMINATE; CHECK_REASON=dp-edid-not-ready; return
    }
    [ "$(json_scalar profileInstalled)" = true ] &&
        [ "$(json_scalar bridgeReady)" = true ] || {
        CHECK_RESULT=FAIL; CHECK_REASON=profile-or-bridge-not-ready; return
    }
    if [ "$(json_scalar running)" != true ]; then
        if [ "$(json_scalar setupRunning)" = true ]; then
            CHECK_RESULT=INDETERMINATE; CHECK_REASON=profile-setup-running
        elif [ "$(json_string preparationState)" = failed ]; then
            CHECK_RESULT=FAIL; CHECK_REASON=profile-preparation-failed
        else
            CHECK_RESULT=INDETERMINATE; CHECK_REASON=session-not-running
        fi
        return
    fi
    session_pid=$(marker_value session.pid)
    session_pgid=$(session_group_pid)
    process_identity "$session_pid" >/dev/null 2>&1 || {
        CHECK_RESULT=FAIL; CHECK_REASON=session-pid-identity-missing; return
    }
    process_identity "$session_pgid" >/dev/null 2>&1 || {
        CHECK_RESULT=FAIL; CHECK_REASON=session-pgid-identity-missing; return
    }
    session_group_token >/dev/null 2>&1 || {
        CHECK_RESULT=FAIL; CHECK_REASON=session-group-token-missing; return
    }
}

renderer_segment_check() {
    renderer_baseline=$1
    renderer_segment=$EVIDENCE/$PHASE-session-segment.log
    valid_decimal "$renderer_baseline" || renderer_baseline=0
    if [ ! -r "$STATE/session.log" ]; then
        CHECK_RESULT=INDETERMINATE
        CHECK_REASON=dev-session-log-unavailable
        return
    fi
    sed -n "$((renderer_baseline + 1)),\$p" "$STATE/session.log" > \
        "$renderer_segment"
    if grep -Eiq 'llvmpipe|lavapipe|swiftshader|software rasterizer' \
            "$renderer_segment"; then
        CHECK_RESULT=FAIL
        CHECK_REASON=software-renderer-observed
    elif grep -Eq 'renderer=vulkan icd=|renderer=gles2 reason=' \
            "$renderer_segment"; then
        :
    else
        CHECK_RESULT=INDETERMINATE
        CHECK_REASON=renderer-selection-not-observed
    fi
}

phase_start() {
    baseline=$(session_log_lines)
    write_atomic "$EVIDENCE/start-session-log-baseline" "$baseline" ||
        record_result FAIL cannot-record-start-baseline

    $CONTROL stop > "$EVIDENCE/start-control-stop.log" 2>&1 ||
        record_result FAIL existing-session-stop-failed
    for setting in \
        'auto_attach 0' \
        "profile $PROFILE" \
        'output_modifiers auto' \
        'direct_scanout auto' \
        'mode_policy preferred' \
        'display_mode auto'; do
        # Values are fixed above and contain no shell metacharacters.
        set -- $setting
        $CONTROL set "$1" "$2" >> "$EVIDENCE/start-control-set.log" 2>&1 ||
            record_result FAIL "setting-$1-failed"
    done
    $CONTROL start-ready-async > "$EVIDENCE/start-control-start.log" 2>&1 ||
        record_result FAIL quick-start-refused

    waited=0
    while [ "$waited" -le "$START_WAIT_SECONDS" ]; do
        load_status || true
        write_snapshot || true
        running_machine_check
        if [ "$CHECK_RESULT" = PASS ]; then
            renderer_segment_check "$baseline"
            record_result "$CHECK_RESULT" "$CHECK_REASON"
        fi
        if [ "$CHECK_RESULT" = FAIL ]; then
            record_result FAIL "$CHECK_REASON"
        fi
        [ "$waited" -ge "$START_WAIT_SECONDS" ] && break
        "$SLEEP_TOOL" "$POLL_SECONDS"
        waited=$((waited + POLL_SECONDS))
    done
    record_result INDETERMINATE start-deadline-without-running-evidence
}

phase_status() {
    load_status || true
    write_snapshot || true
    running_machine_check
    record_result "$CHECK_RESULT" "$CHECK_REASON"
}

phase_stop() {
    load_status || true
    old_pid=$(marker_value session.pid)
    old_pgid=$(session_group_pid)
    old_pid_identity=$(process_identity "$old_pid" 2>/dev/null || true)
    old_pgid_identity=$(process_identity "$old_pgid" 2>/dev/null || true)
    old_display_id=$(marker_value last_dp_display_id)
    {
        printf 'profile=%s\n' "$PROFILE"
        printf 'session_identity=%s\ngroup_identity=%s\n' \
            "${old_pid_identity:-missing}" "${old_pgid_identity:-missing}"
        printf 'external_display_id=%s\n' "${old_display_id:-missing}"
    } > "$EVIDENCE/stop-before.state"

    $CONTROL stop > "$EVIDENCE/stop-control.log" 2>&1 ||
        record_result FAIL control-stop-failed
    waited=0
    while [ "$waited" -le "$STOP_WAIT_SECONDS" ]; do
        load_status || true
        if [ "$(json_scalar running)" != true ]; then break; fi
        [ "$waited" -ge "$STOP_WAIT_SECONDS" ] && break
        "$SLEEP_TOOL" "$POLL_SECONDS"
        waited=$((waited + POLL_SECONDS))
    done
    write_snapshot || true

    [ "$(json_scalar running)" != true ] ||
        record_result FAIL session-still-running-after-stop
    for marker in session.pid session.pgid session-start.intent; do
        [ ! -e "$STATE/$marker" ] ||
            record_result FAIL "stale-$marker-after-stop"
    done
    [ -z "$old_pid_identity" ] || ! same_identity_alive "$old_pid_identity" ||
        record_result FAIL old-session-identity-survived-stop
    [ -z "$old_pgid_identity" ] || ! same_identity_alive "$old_pgid_identity" ||
        record_result FAIL old-group-identity-survived-stop
    if ! valid_decimal "$old_display_id"; then
        record_result INDETERMINATE exact-external-display-id-not-captured
    fi
    stopped_display_state=$(external_display_state "$old_display_id" 2>/dev/null || true)
    if [ "$stopped_display_state" != ON ]; then
        if [ "$(json_string edidState)" != ready ]; then
            record_result INDETERMINATE display-disconnected-during-stop
        fi
        record_result FAIL external-display-not-restored-on
    fi
    record_result PASS stopped-identities-gone-external-display-on
}

phase_before_unplug() {
    load_status || true
    running_machine_check
    [ "$CHECK_RESULT" = PASS ] ||
        record_result "$CHECK_RESULT" "before-unplug-$CHECK_REASON"
    $CONTROL set auto_attach 1 > "$EVIDENCE/before-unplug-control.log" 2>&1 ||
        record_result FAIL enabling-auto-attach-failed
    load_status || true
    write_snapshot || true
    [ "$(json_scalar autoAttach)" = 1 ] ||
        record_result FAIL auto-attach-did-not-persist

    session_pid=$(marker_value session.pid)
    session_pgid=$(session_group_pid)
    session_token=$(session_group_token 2>/dev/null || true)
    session_identity=$(process_identity "$session_pid" 2>/dev/null || true)
    group_identity=$(process_identity "$session_pgid" 2>/dev/null || true)
    [ -n "$session_identity" ] && [ -n "$group_identity" ] &&
        [ -n "$session_token" ] ||
        record_result FAIL exact-running-identity-unavailable
    baseline=$(session_log_lines)
    unplug_state=$EVIDENCE/unplug.state
    {
        printf 'profile=%s\n' "$PROFILE"
        printf 'session_identity=%s\ngroup_identity=%s\n' \
            "$session_identity" "$group_identity"
        printf 'group_token=%s\n' "$session_token"
        printf 'external_display_id=%s\n' \
            "$(marker_value last_dp_display_id)"
        printf 'session_log_baseline=%s\n' "$baseline"
    } > "$unplug_state"
    chmod 0600 "$unplug_state" 2>/dev/null || true
    record_result PASS unplug-evidence-armed-physical-action-not-automated
}

phase_after_unplug() {
    unplug_state=$EVIDENCE/unplug.state
    [ -r "$unplug_state" ] ||
        record_result INDETERMINATE before-unplug-evidence-missing
    [ "$(read_key "$unplug_state" profile)" = "$PROFILE" ] ||
        record_result FAIL before-unplug-profile-mismatch
    old_session=$(read_key "$unplug_state" session_identity)
    old_group=$(read_key "$unplug_state" group_identity)
    old_group_token=$(read_key "$unplug_state" group_token)

    load_status || true
    write_snapshot || true
    [ "$(json_scalar running)" != true ] ||
        record_result FAIL session-running-after-physical-unplug
    [ "$(json_string edidState)" != ready ] ||
        record_result FAIL dp-edid-still-ready-after-physical-unplug
    [ -n "$old_session" ] && ! same_identity_alive "$old_session" ||
        record_result FAIL old-session-identity-survived-unplug
    [ -n "$old_group" ] && ! same_identity_alive "$old_group" ||
        record_result FAIL old-group-identity-survived-unplug
    for marker in session.pid session.pgid session-start.intent; do
        [ ! -e "$STATE/$marker" ] ||
            record_result FAIL "stale-$marker-after-unplug"
    done
    [ "$(marker_value session-state)" = stopped ] ||
        record_result FAIL session-state-not-stopped-after-unplug
    baseline=$(session_log_lines)
    write_atomic "$EVIDENCE/reconnect-session-log-baseline" "$baseline" ||
        record_result FAIL cannot-record-reconnect-baseline
    record_result PASS unplug-cleaned-exact-owned-session
}

phase_after_reconnect() {
    unplug_state=$EVIDENCE/unplug.state
    [ -r "$unplug_state" ] ||
        record_result INDETERMINATE before-unplug-evidence-missing
    old_session=$(read_key "$unplug_state" session_identity)
    old_group=$(read_key "$unplug_state" group_identity)
    old_group_token=$(read_key "$unplug_state" group_token)
    load_status || true
    write_snapshot || true
    running_machine_check
    [ "$CHECK_RESULT" = PASS ] ||
        record_result "$CHECK_RESULT" "reconnect-$CHECK_REASON"
    [ "$(json_scalar autoAttach)" = 1 ] ||
        record_result FAIL reconnect-auto-attach-disabled

    new_session=$(process_identity "$(marker_value session.pid)" 2>/dev/null || true)
    new_group=$(process_identity "$(session_group_pid)" 2>/dev/null || true)
    new_group_token=$(session_group_token 2>/dev/null || true)
    [ -n "$new_session" ] && [ -n "$new_group" ] ||
        record_result FAIL fresh-session-identity-unavailable
    [ "$new_session" != "$old_session" ] ||
        record_result FAIL reconnect-reused-old-session-identity
    [ "$new_group" != "$old_group" ] ||
        record_result FAIL reconnect-reused-old-group-identity
    [ -n "$new_group_token" ] && [ "$new_group_token" != "$old_group_token" ] ||
        record_result FAIL reconnect-reused-or-missing-group-token

    reconnect_baseline=$(cat "$EVIDENCE/reconnect-session-log-baseline" 2>/dev/null)
    valid_decimal "$reconnect_baseline" || reconnect_baseline=0
    renderer_segment_check "$reconnect_baseline"
    [ "$CHECK_RESULT" = PASS ] ||
        record_result "$CHECK_RESULT" "reconnect-$CHECK_REASON"
    record_result PASS fresh-session-after-reconnect-machine-evidence-complete
}

tree_proof() {
    tree_rootfs=$1
    tree_guest=$2
    tree_proof_host=$3
    tree_python='import json, sys
data = json.load(open(sys.argv[1], "r", encoding="utf-8"))
matches = []
def walk(node, output_rect=None):
    if node.get("type") == "output":
        output_rect = node.get("rect")
    app = node.get("app_id") or ""
    name = node.get("name") or ""
    if "vkmark" in (app + " " + name).lower():
        rect = node.get("rect")
        full = int(node.get("fullscreen_mode") or 0)
        exact = int(bool(output_rect) and rect == output_rect)
        matches.append((app, name, full, exact))
    for key in ("nodes", "floating_nodes"):
        for child in node.get(key) or []:
            walk(child, output_rect)
walk(data)
good = [m for m in matches if m[2] > 0 and m[3] == 1]
print("VKMARK_TREE matches=%d fullscreen=%d exact_output=%d" %
      (len(matches), int(bool(good)), int(bool(good))))
sys.exit(0 if good else 1)'
    "$CHROOT_TOOL" "$tree_rootfs" /usr/bin/python3 -c "$tree_python" \
        "$tree_guest" > "$tree_proof_host" 2>&1
}

phase_direct_start() {
    [ "$PROFILE" = sway ] ||
        record_result FAIL direct-scanout-gate-requires-sway
    load_status || true
    write_snapshot || true
    running_machine_check
    [ "$CHECK_RESULT" = PASS ] ||
        record_result "$CHECK_RESULT" "direct-start-$CHECK_REASON"
    [ "$(json_string buildFlavor)" = dev ] &&
        [ "$(json_scalar logsEnabled)" = true ] ||
        record_result INDETERMINATE direct-gate-requires-dev-session-log

    rootfs=$(json_string rootfs)
    case "$rootfs" in /*) ;; *) record_result FAIL invalid-rootfs-path ;; esac
    runtime_guest=/run/user/0
    runtime_host=$rootfs$runtime_guest
    [ -d "$runtime_host" ] && [ ! -L "$runtime_host" ] ||
        record_result FAIL sway-runtime-directory-unavailable
    [ -x "$rootfs$GETFB2_GUEST" ] ||
        record_result INDETERMINATE current-getfb2-probe-not-staged
    [ -x "$rootfs/usr/bin/vkmark" ] ||
        record_result INDETERMINATE vkmark-not-installed
    [ -x "$rootfs/usr/bin/python3" ] ||
        record_result INDETERMINATE python3-tree-parser-not-installed

    sway_socket_host=$(find "$runtime_host" -maxdepth 1 -type s \
        -name 'sway-ipc.*.sock' 2>/dev/null | sed -n '1p')
    [ -n "$sway_socket_host" ] ||
        record_result FAIL sway-ipc-socket-not-found
    sway_socket_guest=${sway_socket_host#"$rootfs"}

    getfb2_guest=$runtime_guest/lindex-gate-getfb2.log
    observer_done_guest=$runtime_guest/lindex-gate-getfb2.done
    vkmark_guest=$runtime_guest/lindex-gate-vkmark.log
    vkmark_done_guest=$runtime_guest/lindex-gate-vkmark.done
    tree_guest=$runtime_guest/lindex-gate-sway-tree.json
    tree_proof_guest=$runtime_guest/lindex-gate-sway-tree.proof
    for exact_file in \
        "$runtime_host/lindex-gate-getfb2.log" \
        "$runtime_host/lindex-gate-getfb2.done" \
        "$runtime_host/lindex-gate-vkmark.log" \
        "$runtime_host/lindex-gate-vkmark.done" \
        "$runtime_host/lindex-gate-sway-tree.json" \
        "$runtime_host/lindex-gate-sway-tree.proof"; do
        rm -f "$exact_file"
    done

    baseline=$(session_log_lines)
    session_identity=$(process_identity "$(marker_value session.pid)" 2>/dev/null || true)
    group_identity=$(process_identity "$(session_group_pid)" 2>/dev/null || true)
    [ -n "$session_identity" ] && [ -n "$group_identity" ] ||
        record_result FAIL exact-running-identity-unavailable
    direct_state=$EVIDENCE/direct.state
    {
        printf 'profile=%s\nrootfs=%s\n' "$PROFILE" "$rootfs"
        printf 'session_identity=%s\ngroup_identity=%s\n' \
            "$session_identity" "$group_identity"
        printf 'session_log_baseline=%s\n' "$baseline"
        printf 'getfb2_host=%s\nobserver_done_host=%s\n' \
            "$runtime_host/lindex-gate-getfb2.log" \
            "$runtime_host/lindex-gate-getfb2.done"
        printf 'vkmark_host=%s\nvkmark_done_host=%s\n' \
            "$runtime_host/lindex-gate-vkmark.log" \
            "$runtime_host/lindex-gate-vkmark.done"
        printf 'tree_host=%s\ntree_proof_host=%s\n' \
            "$runtime_host/lindex-gate-sway-tree.json" \
            "$runtime_host/lindex-gate-sway-tree.proof"
    } > "$direct_state"
    chmod 0600 "$direct_state" 2>/dev/null || true

    "$CHROOT_TOOL" "$rootfs" /usr/bin/swaymsg -s "$sway_socket_guest" \
        debuglog on > "$EVIDENCE/direct-debuglog-command.log" 2>&1 ||
        record_result FAIL sway-debuglog-enable-failed

    observer_command="plane=\${ANDROID_DRM_PRELOAD_PRIMARY_PLANE:-\${DRM_LEASE_OBJECTS##*,}}; i=0; : >$getfb2_guest; while [ \$i -lt 120 ]; do $GETFB2_GUEST --classify \"\$plane\" >>$getfb2_guest 2>&1 || true; i=\$((i+1)); sleep 0.2; done; printf '120\\n' >$observer_done_guest"
    "$CHROOT_TOOL" "$rootfs" /usr/bin/swaymsg -s "$sway_socket_guest" \
        exec "$observer_command" > "$EVIDENCE/direct-observer-command.log" 2>&1 ||
        record_result FAIL same-lease-observer-launch-refused

    vkmark_command="/usr/bin/timeout --signal=TERM --kill-after=5 300 /usr/bin/vkmark --winsys wayland --fullscreen --present-mode mailbox --pixel-format R8G8B8A8_UNORM >$vkmark_guest 2>&1; rc=\$?; printf '%s\\n' \"\$rc\" >$vkmark_done_guest"
    "$CHROOT_TOOL" "$rootfs" /usr/bin/swaymsg -s "$sway_socket_guest" \
        exec "$vkmark_command" > "$EVIDENCE/direct-vkmark-command.log" 2>&1 ||
        record_result FAIL vkmark-launch-refused

    "$SLEEP_TOOL" 3
    "$CHROOT_TOOL" "$rootfs" /usr/bin/swaymsg -s "$sway_socket_guest" \
        -t get_tree -r > "$runtime_host/lindex-gate-sway-tree.json" 2>&1 ||
        record_result FAIL sway-tree-capture-failed
    tree_proof "$rootfs" "$tree_guest" \
        "$runtime_host/lindex-gate-sway-tree.proof" || true
    record_result INDETERMINATE direct-workload-running-run-direct-check-after-completion
}

phase_direct_check() {
    [ "$PROFILE" = sway ] ||
        record_result FAIL direct-scanout-gate-requires-sway
    direct_state=$EVIDENCE/direct.state
    [ -r "$direct_state" ] ||
        record_result INDETERMINATE direct-start-evidence-missing
    [ "$(read_key "$direct_state" profile)" = sway ] ||
        record_result FAIL direct-start-profile-mismatch

    direct_session=$(read_key "$direct_state" session_identity)
    direct_group=$(read_key "$direct_state" group_identity)
    same_identity_alive "$direct_session" ||
        record_result FAIL direct-session-identity-changed
    same_identity_alive "$direct_group" ||
        record_result FAIL direct-group-identity-changed

    getfb2_host=$(read_key "$direct_state" getfb2_host)
    observer_done_host=$(read_key "$direct_state" observer_done_host)
    vkmark_host=$(read_key "$direct_state" vkmark_host)
    vkmark_done_host=$(read_key "$direct_state" vkmark_done_host)
    tree_proof_host=$(read_key "$direct_state" tree_proof_host)
    baseline=$(read_key "$direct_state" session_log_baseline)
    valid_decimal "$baseline" ||
        record_result FAIL invalid-direct-session-log-baseline

    [ -r "$observer_done_host" ] ||
        record_result INDETERMINATE same-lease-observer-still-running
    [ "$(sed -n '1p' "$observer_done_host")" = 120 ] ||
        record_result FAIL same-lease-observer-count-invalid
    [ -r "$vkmark_done_host" ] ||
        record_result INDETERMINATE vkmark-still-running
    vkmark_rc=$(sed -n '1p' "$vkmark_done_host")
    [ "$vkmark_rc" = 0 ] || record_result FAIL "vkmark-exit-$vkmark_rc"
    [ -r "$vkmark_host" ] &&
        grep -Eq 'vkmark Score:[[:space:]]*[0-9]+' "$vkmark_host" ||
        record_result FAIL vkmark-numeric-score-missing
    [ -r "$tree_proof_host" ] &&
        grep -Eq 'VKMARK_TREE matches=[1-9][0-9]* fullscreen=1 exact_output=1' \
            "$tree_proof_host" ||
        record_result FAIL vkmark-not-proven-fullscreen-on-output

    direct_segment=$EVIDENCE/direct-session-segment.log
    [ -r "$STATE/session.log" ] ||
        record_result INDETERMINATE direct-dev-session-log-unavailable
    sed -n "$((baseline + 1)),\$p" "$STATE/session.log" > "$direct_segment"
    grep -F 'Direct scan-out enabled' "$direct_segment" >/dev/null ||
        record_result FAIL upstream-direct-scanout-not-observed

    [ -r "$getfb2_host" ] ||
        record_result INDETERMINATE same-lease-getfb2-log-missing
    cp -f "$getfb2_host" "$EVIDENCE/direct-getfb2.log" ||
        record_result FAIL cannot-collect-getfb2-log
    cp -f "$vkmark_host" "$EVIDENCE/direct-vkmark.log" ||
        record_result FAIL cannot-collect-vkmark-log
    cp -f "$tree_proof_host" "$EVIDENCE/direct-sway-tree.proof" ||
        record_result FAIL cannot-collect-sway-tree-proof
    accepted_count=$(grep -Ec \
        'LEASE_GETFB2 result=(UBWC_PASS|LINEAR_FALLBACK)' \
        "$getfb2_host" 2>/dev/null || true)
    valid_decimal "$accepted_count" || accepted_count=0
    if [ "$accepted_count" -eq 0 ]; then
        if grep -F 'LEASE_GETFB2 plane=' "$getfb2_host" >/dev/null 2>&1; then
            record_result INDETERMINATE getfb2-format-modifier-unclassified
        fi
        record_result FAIL same-lease-active-framebuffer-not-observed
    fi
    record_result PASS ordinary-fullscreen-direct-scanout-and-same-lease-getfb2
}

[ "$#" -eq 2 ] || { usage >&2; exit "$RESULT_USAGE"; }
PHASE=$1
PROFILE=$2
valid_profile "$PROFILE" || { usage >&2; exit "$RESULT_USAGE"; }
safe_runtime_settings || {
    echo 'invalid LinDeX graphics gate timing environment' >&2
    exit "$RESULT_USAGE"
}
[ -x "$CONTROL" ] || {
    echo "LinDeX control is not executable: $CONTROL" >&2
    exit "$RESULT_FAIL"
}

EVIDENCE=$GATE_ROOT/$PROFILE
mkdir -p "$EVIDENCE" || exit "$RESULT_FAIL"
chmod 0700 "$GATE_ROOT" "$EVIDENCE" 2>/dev/null || true

case "$PHASE" in
    start) phase_start ;;
    status) phase_status ;;
    stop) phase_stop ;;
    before-unplug) phase_before_unplug ;;
    after-unplug) phase_after_unplug ;;
    after-reconnect) phase_after_reconnect ;;
    direct-start) phase_direct_start ;;
    direct-check) phase_direct_check ;;
    *) usage >&2; exit "$RESULT_USAGE" ;;
esac

exit "$RESULT_FAIL"
