#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
AUTO=$ROOT/module/bin/auto-service
PROFILE=$ROOT/module/payload/debian/android-drm-profile-manager
ASSET=$ROOT/module/profile-assets/archcraft-sway-free-e4d0126d.tar.gz

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
require_text() { grep -Fq -- "$1" "$2" || fail "missing '$1' in ${2#$ROOT/}"; }

TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/rootfs/run" "$TMP/state" "$TMP/bin"

cat > "$TMP/bin/timeout" <<'EOF'
#!/usr/bin/env bash
shift
exec "$@"
EOF
cat > "$TMP/bin/cmd" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$MOCK_VOLUME_CALLS"
[[ "${1:-}" == media_session && "${2:-}" == volume ]] || exit 64
shift 2
stream=
operation=
value=
while (($#)); do
    case "$1" in
        --stream) stream=${2:-}; shift 2 ;;
        --get) operation=get; shift ;;
        --adj) operation=adjust; value=${2:-}; shift 2 ;;
        --set) operation=set; value=${2:-}; shift 2 ;;
        *) exit 64 ;;
    esac
done
[[ "$stream" == 3 ]] || exit 64
current=$(<"$MOCK_VOLUME_CURRENT")
maximum=$(<"$MOCK_VOLUME_MAXIMUM")
case "$operation" in
    get)
        printf '[V] volume is %s in range [0..%s]\n' "$current" "$maximum"
        ;;
    adjust)
        case "$value" in
            raise) ((current < maximum)) && current=$((current + 1)) ;;
            lower) ((current > 0)) && current=$((current - 1)) ;;
            *) exit 64 ;;
        esac
        printf '%s\n' "$current" > "$MOCK_VOLUME_CURRENT"
        ;;
    set)
        [[ "$value" =~ ^[0-9]+$ ]] || exit 64
        ((value <= maximum)) || exit 64
        printf '%s\n' "$value" > "$MOCK_VOLUME_CURRENT"
        ;;
    *) exit 64 ;;
esac
EOF
chmod 0755 "$TMP/bin/timeout" "$TMP/bin/cmd"

printf '10\n' > "$TMP/current"
printf '15\n' > "$TMP/maximum"
: > "$TMP/calls"
export MOCK_VOLUME_CURRENT=$TMP/current
export MOCK_VOLUME_MAXIMUM=$TMP/maximum
export MOCK_VOLUME_CALLS=$TMP/calls

# Load only the bridge functions from the long-running Android watcher.
awk '
    /^ANDROID_VOLUME_LAST=/ { copy=1 }
    /^# Reconcile the Android hardware-codec broker/ { copy=0 }
    copy { print }
' "$AUTO" > "$TMP/bridge-functions.sh"
STATE=$TMP/state
ROOTFS=$TMP/rootfs
ANDROID_MEDIA_CMD=$TMP/bin/cmd
ANDROID_MEDIA_TIMEOUT=$TMP/bin/timeout
# shellcheck disable=SC1090
source "$TMP/bridge-functions.sh"

process_android_volume_commands
STATE_FILE=$TMP/rootfs/run/android-drm/android-volume.state
COMMANDS=$TMP/rootfs/run/android-drm/android-volume-commands
require_text 'volume=10' "$STATE_FILE"
require_text 'maximum=15' "$STATE_FILE"
[[ $(stat -c '%a' "$TMP/rootfs/run/android-drm") == 700 ]] ||
    fail 'shared Android DRM runtime permissions were weakened'
[[ $(stat -c '%a' "$COMMANDS") == 700 ]] ||
    fail 'volume command queue is not private'
[[ $(stat -c '%a' "$STATE_FILE") == 600 ]] ||
    fail 'volume state is not private'
[[ $(<"$TMP/state/android-volume-last-nonzero") == 10 ]] ||
    fail 'initial nonzero volume was not remembered'
last_inode=$(stat -c '%i' "$TMP/state/android-volume-last-nonzero")
publish_android_media_volume
[[ $(stat -c '%i' "$TMP/state/android-volume-last-nonzero") == "$last_inode" ]] ||
    fail 'unchanged nonzero volume rewrote persistent state'

printf 'lower\n' > "$COMMANDS/01.cmd"
process_android_volume_commands
[[ $(<"$TMP/current") == 9 ]] || fail 'scroll-down did not lower STREAM_MUSIC'
printf 'raise\n' > "$COMMANDS/02.cmd"
process_android_volume_commands
[[ $(<"$TMP/current") == 10 ]] || fail 'scroll-up did not raise STREAM_MUSIC'
printf 'toggle\n' > "$COMMANDS/03.cmd"
process_android_volume_commands
[[ $(<"$TMP/current") == 0 ]] || fail 'click did not mute STREAM_MUSIC'
printf 'toggle\n' > "$COMMANDS/04.cmd"
process_android_volume_commands
[[ $(<"$TMP/current") == 10 ]] || fail 'second click did not restore STREAM_MUSIC'

printf 'raise with unbounded trailing payload\n' > "$COMMANDS/oversized.cmd"
ln -s "$TMP/current" "$COMMANDS/symlink.cmd"
process_android_volume_commands
[[ $(<"$TMP/current") == 10 ]] || fail 'invalid command changed STREAM_MUSIC'
[[ ! -e "$COMMANDS/oversized.cmd" && ! -L "$COMMANDS/symlink.cmd" ]] ||
    fail 'invalid command entries were not cleaned'

printf '0\n' > "$TMP/current"
printf '30\n' > "$TMP/maximum"
for number in $(seq -w 1 10); do printf 'raise\n' > "$COMMANDS/$number.cmd"; done
process_android_volume_commands
[[ $(<"$TMP/current") == 8 ]] || fail 'watcher did not bound one queue pass to eight actions'
[[ $(find "$COMMANDS" -maxdepth 1 -name '*.cmd' -type f | wc -l) == 2 ]] ||
    fail 'bounded queue pass consumed the wrong number of actions'
process_android_volume_commands
[[ $(<"$TMP/current") == 10 ]] || fail 'deferred queue actions were not processed'

extract_heredoc() {
    destination=$1
    marker=$2
    awk -v marker="$marker" '
        index($0, marker) { copy=1; next }
        copy && $0 == "EOF" { exit }
        copy { print }
    ' "$PROFILE" > "$destination"
    chmod 0755 "$destination"
}

extract_heredoc "$TMP/android_volume" 'scripts/android_volume" <<'
extract_heredoc "$TMP/android_volume_control" 'scripts/android_volume_control" <<'
extract_heredoc "$TMP/startup" 'scripts/startup" <<'
bash -n "$TMP/android_volume" "$TMP/android_volume_control" "$TMP/startup"
output=$(ANDROID_VOLUME_STATE=$STATE_FILE "$TMP/android_volume")
[[ "$output" == *'33%' ]] || fail "Waybar volume output is unexpected: $output"
ANDROID_VOLUME_COMMANDS=$COMMANDS "$TMP/android_volume_control" lower
queued_command=$(find "$COMMANDS" -maxdepth 1 -name '*.cmd' -type f | head -1)
[[ -n "$queued_command" && $(<"$queued_command") == lower ]] ||
    fail 'Waybar control did not queue scroll-down'
rm -f "$COMMANDS"/*.cmd
if ANDROID_VOLUME_COMMANDS=$COMMANDS "$TMP/android_volume_control" invalid; then
    fail 'Waybar control accepted an unbounded action'
fi
if grep -Eq '(^|[^[:alnum:]_])(aplay|amixer|pactl|pulsemixer)([^[:alnum:]_]|$)|/dev/snd' \
    "$TMP/android_volume" "$TMP/android_volume_control"; then
    fail 'Waybar volume bridge opens or controls a Linux audio path'
fi
if grep -Fq 'run mpd' "$TMP/startup"; then
    fail 'Sway startup still launches an audio daemon'
fi

# Apply the real adapter to the checksum-pinned public Waybar tree, including
# a second pass to catch duplicate objects and other non-idempotent edits.
tar -xzf "$ASSET" -C "$TMP"
WAYBAR_ROOT=$TMP/archcraft-sway-free/files
WAYBAR_CONFIG=$WAYBAR_ROOT/waybar/config
WAYBAR_MODULES=$WAYBAR_ROOT/waybar/modules
WAYBAR_STYLE=$WAYBAR_ROOT/waybar/style.css
WAYBAR_LIGHTMODE=$WAYBAR_ROOT/waybar/lightmode
sed -i 's/\r$//' "$WAYBAR_CONFIG" "$WAYBAR_MODULES" "$WAYBAR_STYLE" \
    "$WAYBAR_LIGHTMODE"
awk '
    /^adapt_sway_waybar_for_android\(\)/ { copy=1 }
    /^normalize_profile_text_files\(\)/ { copy=0 }
    copy { print }
' "$PROFILE" > "$TMP/waybar-adapter.sh"
# shellcheck disable=SC1090
source "$TMP/waybar-adapter.sh"
adapt_sway_waybar_for_android "$WAYBAR_ROOT"
adapt_sway_waybar_for_android "$WAYBAR_ROOT"
require_text '' "$WAYBAR_LIGHTMODE"
if grep -Fq '󰖨' "$WAYBAR_LIGHTMODE"; then
    fail 'Nerd Font-only light-mode icon survived the Debian adapter'
fi
require_text '"custom/android-volume"' "$WAYBAR_CONFIG"
if ! grep -Fq 'scripts/android_volume_control toggle' "$WAYBAR_MODULES"; then
    tail -60 "$WAYBAR_MODULES" >&2
    fail 'Waybar Android volume object was not inserted'
fi
require_text 'scripts/android_volume_control raise' "$WAYBAR_MODULES"
require_text 'scripts/android_volume_control lower' "$WAYBAR_MODULES"
require_text '#custom-android-volume' "$WAYBAR_STYLE"
[[ $(grep -Fc '"custom/android-volume": {' "$WAYBAR_MODULES") == 1 ]] ||
    fail 'Waybar Android volume object is missing or duplicated'
modules_right=$(grep '"modules-right"' "$WAYBAR_CONFIG")
for removed in pulseaudio backlight bluetooth; do
    [[ "$modules_right" != *"\"$removed\""* ]] ||
        fail "Waybar still instantiates $removed"
done
[[ $(grep '"modules-left"' "$WAYBAR_CONFIG" | head -1) != *'"mpd"'* ]] ||
    fail 'Waybar still instantiates MPD'
python3 - "$WAYBAR_CONFIG" "$WAYBAR_MODULES" <<'PY'
import json
import re
import sys

def strip_line_comments(text):
    output = []
    quoted = escaped = False
    index = 0
    while index < len(text):
        char = text[index]
        if quoted:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            index += 1
            continue
        if char == '"':
            quoted = True
            output.append(char)
            index += 1
        elif char == "/" and index + 1 < len(text) and text[index + 1] == "/":
            index = text.find("\n", index)
            if index < 0:
                break
        else:
            output.append(char)
            index += 1
    return "".join(output)

for path in sys.argv[1:]:
    text = open(path, encoding="utf-8").read()
    text = strip_line_comments(text)
    text = re.sub(r",\s*([}\]])", r"\1", text)
    try:
        json.loads(text)
    except json.JSONDecodeError as error:
        lines = text.splitlines()
        first = max(0, error.lineno - 3)
        last = min(len(lines), error.lineno + 2)
        context = "\n".join(
            f"{number + 1}: {lines[number]}" for number in range(first, last)
        )
        raise SystemExit(f"{path}: {error}\n{context}") from error
PY

if grep -Ev '^[[:space:]]*#' "$PROFILE" "$AUTO" |
   grep -Eq 'mount[^#]*(/system|/apex)|mount[^#]*/system/bin/cmd'; then
    fail 'volume bridge mounts Android system paths into the chroot'
fi
require_text '"$ANDROID_MEDIA_TIMEOUT" 2 "$ANDROID_MEDIA_CMD"' "$AUTO"
require_text 'chmod 0700 "$ANDROID_VOLUME_RUNTIME" "$ANDROID_VOLUME_COMMANDS"' "$AUTO"

printf 'LinDeX Android STREAM_MUSIC bridge tests passed\n'
