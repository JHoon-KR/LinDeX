# Troubleshooting

[English](TROUBLESHOOTING.md) | [한국어](ko/TROUBLESHOOTING.md)

Start every recovery with DP physically detached. Use a dev build only when a
bounded diagnostic is necessary; release mode intentionally keeps no persistent
diagnostic log.

## WebUI does not open after an update

1. Confirm the KernelSU installer reported success.
2. Confirm you rebooted Android normally after installing the module.
3. Leave DP detached and reopen the module WebUI.
4. If the module is disabled or installation checks failed, do not force-start
   binaries from a terminal; reinstall the exact verified ZIP.

## KernelSU installer reports `illegal mode: 022` or visible `\r`

The module scripts and release ZIP use LF line endings. This error can instead
come from a custom Windows-built `ksud` whose embedded `installer.sh` was
compiled with CRLF line endings. Update or rebuild KernelSU userspace from an LF
checkout. Do not modify Android read-only partitions and do not add a `system/`
overlay to LinDeX as a workaround. The reference-device acceptance used an
ephemeral copy of the same `ksud` with only its embedded installer text
normalized; the installed KernelSU binary was not changed.

## Profile setup fails

Read the short state first:

- `dependency-required`: a signed Debian dependency is unavailable.
- `profile-unavailable`: a required bundled profile asset is missing or failed
  checksum/source-lock validation.
- `dependency-install-failed`: package or archive installation failed.
- `missing-pywal-command`: the pinned `wal` provider required by the Sway
  Pywal choice is unavailable; select Dark/Light or restore the verified
  provider input.
- checksum or bridge error: do not bypass it; replace the release input.

Retry only after network/package availability or the release asset is corrected.
For Archcraft Sway Free, reinstall the exact verified LinDeX release so its
official public asset bundle is restored. Do not use an unofficial binary or a
private/paid Ko-fi archive to satisfy a runtime error. The linked Ko-fi page is
optional support for the Archcraft creator, never a repair or install source.

## DP is connected but no monitor is ready

- Confirm the phone, cable, adapter, and monitor support USB-C DP Alt Mode.
- Wait for a physically connected connector and valid EDID in the WebUI.
- Return to an EDID-advertised mode. Manual high-refresh modes are experimental.
- Stop the session, detach DP, and reconnect. A new session and lease should be
  created.

## Monitor is blank after start

1. Disconnect DP and press **Stop** in the WebUI.
2. Disable direct scanout with the compatibility `off` option and retry an
   EDID-advertised mode.
3. Keep Vulkan/GLES selection on the live-probed default unless a dev result
   identifies a specific backend failure.
4. Do not install an old compositor patch or remount Android partitions as a
   workaround.

If a dev log says `Could not match drm and vulkan device`, verify the immutable
runtime includes `libandroid-vulkan-drm-identity-layer.so`, its explicit-layer
JSON manifest, and that runtime checksum
verification passed. Do not set its acknowledgement by hand: the stock session
owns it only after the hardware Turnip probe, and the frontend still requires
the exact device identities documented in
[Vulkan DRM identity bridge](VULKAN_DRM_IDENTITY.md). A rejection should follow
the normal GLES2 fallback instead of being bypassed with a guessed DRM node.

## USB input does not return to Android

Disconnect DP and stop the exact LinDeX session. The final owned descriptor
should release the kernel grab. Do not kill global input services. If it repeats,
reproduce with a dev build and record whether the WebUI was in `shared` or
`linux-exclusive` mode and which USB event type was affected.

## Android cannot re-enable Wi-Fi after a Sway session

LinDeX removes the Sway Waybar network module and `rofi_network` helper. It does
not read `wlan*` or route state, and it does not start NetworkManager,
`nm-applet`, a supplicant, or a Wi-Fi/rfkill menu in the chroot.
First distinguish ownership from a vendor-driver failure:

- an Android setting of `wifi_on=1` together with `cmd wifi status` reporting
  disabled means Android accepted the request but did not finish starting Wi-Fi;
- `numSetupClientInterfaceFailureDueToHal` increasing while the wificond and
  supplicant failure counters remain zero identifies the HAL boundary; and
- `CNSS` idle-restart timeout, `is_driver_recovering 1`, or `Failed to start
  WLAN modules` in the kernel log identifies a Qualcomm firmware/driver stall,
  not chroot ownership.

Do not write guessed values to `/sys/kernel/cnss`, unload vendor modules, or
restart Wi-Fi services automatically from LinDeX. Capture a bounded dev report,
stop the session, and use an explicitly approved Android Wi-Fi-stack recovery
or reboot. A release profile must contain no active network widget,
`nm-applet` startup, or `rofi_network` helper.

## Video acceleration is unavailable

This can be the correct fail-closed result. Strict zero-copy options appear only
after the exact live backend probe passes. Decode PRIME is intentionally
disabled until the public validation matrix closes. Do not force a capability
bit or treat byte/AHB evidence as PRIME validation.

If FFmpeg and `vainfo` list ADVC but GStreamer reports no `vaapih264enc` or
`vaapih265enc`, start a new LinDeX session; do not kill a plugin-scanner
process. The verified session uses a transient task-owned registry and sets
`GST_VAAPI_ALL_DRIVERS=1` and
`GST_REGISTRY_FORK=no` inside the successful ADVC gate because the forked
plugin scanner can lose the VA-API feature reply in this Android/chroot setup.
Do not export these variables globally to make a failed ADVC preflight appear
successful.

For an opt-in, bounded H.264 decode comparison in a dev checkout, run this
inside an already validated LinDeX session:

```sh
./scripts/diagnose-gstreamer-h264-decode.sh --timeout 30 --output-dir /tmp/lindex-gst-h264-report /path/to/sample.h264
```

The tool runs `avdec_h264` and `vaapih264dec` against the same input, uses its
own transient registry with scanner forking disabled, and removes that registry
on exit. It leaves raw logs and a `summary.txt` that conservatively classifies
the parser-EOS, decoder finish-frame, downstream-buffer, downstream-EOS, and bus
EOS boundaries. A classification is evidence about the last visible boundary,
not proof of the blocked call; retain the raw logs. The command never installs
plugins, enables a failed ADVC gate, contacts Android through ADB, or kills a
plugin scanner. `h264parse`, `avdec_h264`, and `vaapih264dec` must already be
available in the dev session.

If the bus EOS message is present but the bounded process still times out, the
summary reports `post-bus-eos-process-timeout`. This is a symptom boundary, not
proof of MediaCodec teardown. Check for accidental closure of the process poll
FDs as well as codec stop traces. LinDeX fixed one such case where dma-buf
cleanup closed undeclared FD 0. A pass still requires process exit within the
bound, destroy drain disabled, zero discarded outputs and codec stop status 0.

## What to include in a report

- LinDeX version, Git commit if known, ZIP SHA-256, and build flavor;
- device model, Android version, GPU family, dock/adapter model without serials;
- selected profile and WebUI settings;
- exact start/stop/connect order; and
- the smallest sanitized dev-log excerpt or WebUI error.

Do not include a device serial, authentication token, cookie, complete Android
log, or paid third-party file. Security-sensitive problems should follow
[SECURITY.md](../SECURITY.md).
