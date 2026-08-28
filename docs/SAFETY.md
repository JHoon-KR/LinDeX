# Safety and recovery

[English](SAFETY.md) | [한국어](ko/SAFETY.md)

## Non-negotiable rules

- Never write to or remount Android `/system`, `/vendor`, or `/product` for
  LinDeX. The module creates no `/system` overlay.
- Physically detach DP before installing or updating the module.
- Activate module changes with a normal Android reboot.
- Back up `/data/local/debian` before updating or uninstalling. Uninstall
  intentionally removes the chroot and its contents.
- Use the WebUI **Stop** action before removal or risky display-setting changes.
- Do not run broad process-name kills or broad Android display-disable commands.
- Strict zero-copy mode must never silently fall back to a CPU raw-pixel copy.
- Treat manual 100/120/144 Hz modes as experimental; they have no automatic
  rollback.

## Automatic cleanup

Each monitor session owns one exact PID/process group and DRM lease. Normal
stop, compositor failure, and physical cable removal close that session's
descriptors and terminate only its process group. USB input grabs are tied to
the same session descriptor lifetime. A root-only per-session token follows
the launcher descendants, allowing cleanup to terminate a DRM-holding child
even if its group leader exits first. Hard connector removal bypasses the EDID
debounce; reconnect waits for two stable DP+EDID samples.

Reconnect must create a fresh lease and session. If it does not, leave DP
detached and use the troubleshooting flow rather than killing unrelated Android
or Debian processes.

## Logging and privacy

Release packages create no persistent setup/session/codec diagnostics and
remove stale equivalents. Dev packages retain bounded rotated diagnostics.
Before sharing a dev excerpt, remove device serials, account tokens, cookies,
local paths, unrelated process data, and complete Android logs.

## Recovery order

1. Physically disconnect DP.
2. Open the WebUI and press **Stop**.
3. Wait for the state to report stopped and confirm USB input returned.
4. Reconnect DP only after a valid EDID is shown.
5. If the failure repeats, keep DP detached and reproduce once with a dev build.
6. Collect only the bounded relevant error and follow
   [Troubleshooting](TROUBLESHOOTING.md).

If the WebUI is unavailable but the module command is present, the scoped stop
command is:

```sh
su -c 'debian-gpu-control stop'
```

Do not improvise commands that disable every external display or kill every
process with a matching executable name.
