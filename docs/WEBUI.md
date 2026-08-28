# WebUI guide

[English](WEBUI.md) | [한국어](ko/WEBUI.md)

The LinDeX WebUI is the supported control surface for normal installation and
monitor-session use. End users should not need to copy a compositor binary,
edit module state, or search Android storage for a profile archive.

## Main controls

- **Profile:** Archcraft Sway Free, LXQt + stock labwc, or XFCE + stock labwc.
- **Sway theme:** when Archcraft Sway Free is selected, choose **Official
  Dark** (default), **Official Light**, or **Auto-generated · Pywal**. Pywal
  derives colors from bundled or user wallpapers on the next start.
- **Start now / Stop:** prepares the selected profile and owns one bounded
  compositor process group and DRM lease.
- **Resolution and refresh rate:** reads modes from the connected monitor.
  Manual 100/120/144 Hz choices are experimental.
- **Automatic DP start:** starts a prepared profile only after physical DP and
  checksum-valid EDID are present.
- **XWayland:** enables legacy X11 application support when needed.
- **Phone touch sharing:** off by default; the internal touchscreen stays with
  Android unless the user explicitly changes this setting.
- **USB dock input:** `linux-exclusive` grabs eligible USB keyboard, mouse, and
  indirect touchpad event nodes only for the active monitor session; `shared`
  leaves them visible to both environments.
- **Mesa:** **KGSL + patched Turnip** is the recommended default and uses only
  the standard Mesa archive, which already contains KGSL OpenGL/GLES and the
  patched Turnip Vulkan driver. **KGSL + unpatched Turnip** applies the separate
  compatibility override only when the standard Turnip does not work. Hardware
  Vulkan probing falls back to KGSL GLES automatically.
- **Output modifiers:** `UBWC preference` is the new-install default and is
  activated only after exact-device validation. A failed validation preserves
  the original device formats. `auto` preserves existing choices and only appends the
  validated XB24/QCOM candidate; it is compatibility-first and does not prefer
  UBWC. `LINEAR` disables modifiers. `UBWC preference` narrows the exact
  validated device path so eligible fullscreen clients can select XB24/QCOM.
  It does not turn the normal XR24 composited desktop into UBWC.
- **Direct scanout:** `auto` lets the compositor decide; `off` is a compatibility
  mode. No force-on option exists. This selector controls scene eligibility,
  not output-buffer format preference.
- **Video acceleration:** selects the policy exposed by the current live codec
  capabilities. Unsupported strict paths remain unavailable.

## First-start stages

The progress panel can show rootfs checks, profile asset checks, Debian package
installation, configuration, Mesa setup, and bridge readiness. Archcraft Sway
Free uses the checksum-locked official public Archcraft configuration and
appearance assets bundled with the release; it never asks for a Ko-fi archive.
Wait for either `ready` or a specific failure state before reconnecting DP or
retrying. Repeated blind starts can hide the first useful error.

The WebUI shows short, sanitized states rather than complete device logs.
Release packages do not retain persistent setup/session/codec logs. If a
problem requires diagnostics, reproduce it once with the matching dev package,
collect only the bounded relevant excerpt, and remove tokens, serials, and
unrelated device data.

## Safe operating order

1. Install or update with DP physically detached.
2. Reboot Android normally to activate the root module.
3. Open WebUI and verify build flavor and rootfs state.
4. Select a profile. For Sway, select Dark, Light, or Pywal, then press
   **Start now** and wait for setup to finish. Pywal invokes the installed
   official script `~/.config/sway/theme/theme.sh --pywal`.
5. Connect DP when prompted and start the session if it is not already waiting
   for the display.
6. Stop the session before updating, removing the module, or changing risky
   experimental display settings.

## Recovery

If the monitor is blank or a session does not stop, disconnect DP, return to
the WebUI, and press **Stop**. Do not run broad process-kill or Android display
commands. See [Troubleshooting](TROUBLESHOOTING.md) and
[Safety and recovery](SAFETY.md).
