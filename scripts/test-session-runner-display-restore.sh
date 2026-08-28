#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
RUNNER=$REPO/module/bin/session-runner
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

fail() {
    echo "session-runner restore test: $*" >&2
    exit 1
}

grep -Fq 'trap finish_session_runner EXIT' "$RUNNER" ||
    fail 'session runner does not use the release-log cleanup trap'
grep -A8 -F 'finish_session_runner() {' "$RUNNER" | grep -Fq 'stop_session_group' ||
    fail 'EXIT cleanup does not reconcile the owned compositor process group'
grep -A3 -F 'wait "$CHILD_PID"' "$RUNNER" | grep -Fq 'stop_session_group' ||
    fail 'normal launcher exit does not reconcile token-owned orphan children'
grep -Fq 'LINDEX_SESSION_TOKEN=$SESSION_TOKEN' "$RUNNER" ||
    fail 'session descendants do not inherit an ownership token'
grep -Fq '[ "$BUILD_FLAVOR" = dev ] || rm -f "$DISPLAY_CMD_LOG"' "$RUNNER" ||
    fail 'release session does not remove its temporary DisplayManager log'
grep -Fq 'while session_running && [ "$attempt" -lt 30 ]' "$REPO/module/bin/common.sh" ||
    fail 'desktop stop timeout is shorter than the display recovery window'

mkdir -p "$TMP/bin"
MOCK_ACTION_LOG=$TMP/actions.log
MOCK_STATE=$TMP/state
MOCK_CONNECTED=$TMP/connected
MOCK_ENABLE_COUNT=$TMP/enable-count
MOCK_SLEEP_COUNT=$TMP/sleep-count
MOCK_DISPLAY_ID=18
export MOCK_ACTION_LOG MOCK_STATE MOCK_CONNECTED MOCK_ENABLE_COUNT
export MOCK_DISPLAY_ID MOCK_ENABLE_MODE MOCK_HIDE_OFF MOCK_DUMP_NOISE
export MOCK_SLEEP_COUNT

cat > "$TMP/bin/cmd" <<'EOF'
#!/bin/sh
set -eu

[ "$1" = display ] || exit 64
shift
case "$1" in
    get-displays)
        [ "$(cat "$MOCK_CONNECTED")" = 1 ] || exit 0
        state=$(cat "$MOCK_STATE")
        if [ "$MOCK_HIDE_OFF" = 1 ] && [ "$state" = OFF ]; then
            exit 0
        fi
        case " $* " in
            *" --ids-only "*) printf '%s\n' "$MOCK_DISPLAY_ID" ;;
            *)
                printf 'Displays:\nDisplay id %s: DisplayInfo{"External", displayId %s, state %s, committedState %s, type EXTERNAL}\n' \
                    "$MOCK_DISPLAY_ID" "$MOCK_DISPLAY_ID" "$state" "$state"
                ;;
        esac
        ;;
    disable-display)
        printf 'disable-display %s\n' "$2" >> "$MOCK_ACTION_LOG"
        printf 'OFF\n' > "$MOCK_STATE"
        ;;
    enable-display)
        printf 'enable-display %s\n' "$2" >> "$MOCK_ACTION_LOG"
        count=$(cat "$MOCK_ENABLE_COUNT")
        count=$((count + 1))
        printf '%s\n' "$count" > "$MOCK_ENABLE_COUNT"
        case "$MOCK_ENABLE_MODE" in
            normal|delayed-off) printf 'ON\n' > "$MOCK_STATE" ;;
            first-stuck)
                [ "$count" -eq 1 ] || printf 'ON\n' > "$MOCK_STATE"
                ;;
            initial-fail)
                if [ "$count" -eq 1 ]; then
                    exit 1
                fi
                printf 'ON\n' > "$MOCK_STATE"
                ;;
            always-off) printf 'OFF\n' > "$MOCK_STATE" ;;
            nochange) ;;
            disconnect) printf '0\n' > "$MOCK_CONNECTED" ;;
            *) exit 65 ;;
        esac
        ;;
    power-reset)
        printf 'power-reset %s\n' "$2" >> "$MOCK_ACTION_LOG"
        [ "$MOCK_ENABLE_MODE" = always-off ] || printf 'ON\n' > "$MOCK_STATE"
        ;;
    *) exit 64 ;;
esac
EOF

cat > "$TMP/bin/dumpsys" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = display ]
printf 'DISPLAY MANAGER (dumpsys display)\n'
[ "$(cat "$MOCK_CONNECTED")" = 1 ] || exit 0
state=$(cat "$MOCK_STATE")
if [ "${MOCK_DUMP_NOISE:-0}" -gt 0 ]; then
    awk -v count="$MOCK_DUMP_NOISE" 'BEGIN {
        for (i = 0; i < count; i++)
            printf "  unrelated-display-diagnostic-%06d=%0800d\n", i, i
    }'
fi
printf '  Display %s:\n    mDisplayInfo=DisplayInfo{"External", displayId %s, state %s, committedState %s, type EXTERNAL}\n' \
    "$MOCK_DISPLAY_ID" "$MOCK_DISPLAY_ID" "$state" "$state"
EOF

cat > "$TMP/bin/su" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = 2000 ]
[ "$2" = -c ]
exec /bin/sh -c "$3"
EOF
chmod +x "$TMP/bin/cmd" "$TMP/bin/dumpsys" "$TMP/bin/su"

awk '
    /^captured_external_display_line\(\)/ { emit = 1 }
    /^terminate_session\(\)/ { emit = 0 }
    emit { print }
' "$RUNNER" |
    sed \
        -e "s|/system/bin/cmd|$TMP/bin/cmd|g" \
        -e "s|/system/bin/dumpsys|$TMP/bin/dumpsys|g" \
        -e "s|/system/bin/su|$TMP/bin/su|g" \
        -e 's/sleep "$restore_delay"/mock_sleep "$restore_delay"/g' \
        -E -e 's/sleep "?([0-9]+)"?/mock_sleep \1/g' > "$TMP/restore-functions.sh"

# shellcheck disable=SC1090
. "$TMP/restore-functions.sh"
clear_owned_start_intent() { :; }
physical_dp_path() { [ "$(cat "$MOCK_CONNECTED")" = 1 ]; }
mock_sleep() {
    count=$(cat "$MOCK_SLEEP_COUNT")
    count=$((count + 1))
    printf '%s\n' "$count" > "$MOCK_SLEEP_COUNT"
    # Reproduce the Samsung policy race observed on-device: the external
    # display can fall OFF around 9-10 seconds after an apparently good ON.
    if [ "$MOCK_ENABLE_MODE" = delayed-off ] && [ "$count" -eq 4 ]; then
        printf 'OFF\n' > "$MOCK_STATE"
    fi
}
RESTORE_ANDROID_POLICY=1
DISPLAY_CMD_LOG=$TMP/display-command.log
DISPLAY_ID=$MOCK_DISPLAY_ID

run_case() {
    MOCK_ENABLE_MODE=$1
    MOCK_HIDE_OFF=$4
    export MOCK_ENABLE_MODE MOCK_HIDE_OFF
    printf '%s\n' "$2" > "$MOCK_STATE"
    printf '%s\n' "$3" > "$MOCK_CONNECTED"
    printf '0\n' > "$MOCK_ENABLE_COUNT"
    printf '0\n' > "$MOCK_SLEEP_COUNT"
    : > "$MOCK_ACTION_LOG"
    rm -f "$DISPLAY_CMD_LOG"
    RESTORING=0
    MOCK_DUMP_NOISE=${5:-0}
    export MOCK_DUMP_NOISE
    restore_android_display
}

assert_actions() {
    expected=$1
    actual=$(cat "$MOCK_ACTION_LOG")
    [ "$actual" = "$expected" ] || {
        printf 'expected actions:\n%s\nactual actions:\n%s\n' "$expected" "$actual" >&2
        fail "unexpected display action sequence"
    }
}

output=$(run_case normal OFF 1 0)
printf '%s\n' "$output" | grep -Fq 'restored state=ON' ||
    fail "normal restore was not verified ON"
assert_actions 'enable-display 18'

output=$(run_case delayed-off OFF 1 0)
printf '%s\n' "$output" | grep -Fq 'restore was transient; observed state=OFF' ||
    fail "delayed HWC OFF transition was not detected"
printf '%s\n' "$output" | grep -Fq 'recovered state=ON' ||
    fail "delayed HWC OFF transition was not recovered"
assert_actions 'enable-display 18
disable-display 18
enable-display 18
power-reset 18'

output=$(run_case first-stuck OFF 1 1)
printf '%s\n' "$output" | grep -Fq 'remains OFF; running exact-ID recovery' ||
    fail "stuck OFF state did not enter targeted recovery"
printf '%s\n' "$output" | grep -Fq 'recovered state=ON' ||
    fail "targeted recovery was not verified ON"
assert_actions 'enable-display 18
disable-display 18
enable-display 18
power-reset 18'

output=$(run_case initial-fail OFF 1 0)
printf '%s\n' "$output" | grep -Fq 'initial restore command failed' ||
    fail "initial command failure was not reported"
printf '%s\n' "$output" | grep -Fq 'recovered state=ON' ||
    fail "OFF state after command failure was not recovered"
assert_actions 'enable-display 18
disable-display 18
enable-display 18
power-reset 18'

output=$(run_case always-off OFF 1 0)
printf '%s\n' "$output" | grep -Fq 'recovery verification state=OFF' ||
    fail "persistent OFF state did not fail verification"
assert_actions 'enable-display 18
disable-display 18
enable-display 18
power-reset 18'

output=$(run_case nochange DOZE 1 0)
printf '%s\n' "$output" | grep -Fq 'state=UNKNOWN Display ID=18; targeted recovery skipped' ||
    fail "unknown state did not fail closed"
assert_actions 'enable-display 18'

output=$(run_case disconnect OFF 1 0)
printf '%s\n' "$output" | grep -Fq 'state=DISCONNECTED Display ID=18; targeted recovery skipped' ||
    fail "disconnected captured display did not skip recovery"
assert_actions 'enable-display 18'

output=$(run_case normal OFF 0 0)
printf '%s\n' "$output" | grep -Fq 'physical DP/EDID absent; exact-ID restore skipped' ||
    fail "physical unplug did not suppress exact-ID restore"
assert_actions ''

# Force the dumpsys fallback and make its output larger than a typical ARG_MAX.
# The helper must stream the dump and still find the exact external display.
output=$(run_case first-stuck OFF 1 1 4000)
printf '%s\n' "$output" | grep -Fq 'recovered state=ON' ||
    fail "large display dump was not streamed safely"
assert_actions 'enable-display 18
disable-display 18
enable-display 18
power-reset 18'

echo 'session-runner exact display restore checks: PASS'
