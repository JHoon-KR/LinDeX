# Vulkan DRM identity bridge

[English](VULKAN_DRM_IDENTITY.md) | [한국어](ko/VULKAN_DRM_IDENTITY.md)

Turnip on the validated Android KGSL reference stack is a working hardware
Vulkan renderer, but Mesa 26.2-devel does not advertise
`VK_EXT_physical_device_drm`. Unmodified wlroots 0.18 and 0.20 therefore cannot
pair that Vulkan physical device with the leased KMS device and report
`Could not match drm and vulkan device`.

`libandroid-vulkan-drm-identity-layer.so` is a narrowly scoped standard Vulkan
explicit layer; it is not a wlroots, Sway, or Mesa patch. The stock session
enables it with `VK_LAYER_PATH` and `VK_INSTANCE_LAYERS`. It intercepts
device-extension enumeration and the core/KHR
`vkGetPhysicalDeviceProperties2` entry points through Vulkan loader dispatch.
If the driver already exposes
`VK_EXT_physical_device_drm`, every native result is passed through without
adding or replacing anything.

## Fail-closed gate

Synthetic identity is enabled only when all three of these session-owned
values are exact:

```text
ANDROID_VULKAN_DRM_IDENTITY_ENABLE=1
ANDROID_VULKAN_DRM_IDENTITY_ACK=turnip-qualcomm-card0-renderD128-kgsl3d0-<primary-major>-<primary-minor>-<render-major>-<render-minor>-<kgsl-major>-<kgsl-minor>-v2
ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID=<compositor-pid>
```

The acknowledgement alone is insufficient. The owner PID must equal the
calling process PID, which confines synthetic identity to the compositor even
when desktop applications inherit the layer environment. For each Vulkan
physical device,
the frontend also requires all of the following live facts:

- Qualcomm vendor ID `0x5143`, Mesa Turnip driver ID and a Turnip driver name;
- `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, and
  `VK_EXT_image_drm_format_modifier`;
- real, non-symlink character devices at the three fixed paths
  `/dev/dri/card0`, `/dev/dri/renderD128`, and `/dev/kgsl-3d0`, each matching
  the exact live `st_rdev` tuple embedded in the v2 acknowledgement.

Only then is `VK_EXT_physical_device_drm` appended and its requested properties
filled with the measured primary and render tuples. Android can allocate KGSL's
character-device major dynamically after a boot, so the session reads the
three exact paths instead of assuming the reference phone's former `466:0`.
A missing callback, extension, node, exact rdev, vendor, driver, allocation, or
acknowledgement returns the original Vulkan result. The code never scans for or
guesses substitute devices.

## Runtime behavior

The stock profile session adds the layer only after `vulkaninfo` selects a
non-software Freedreno/Turnip ICD. An unavailable Vulkan renderer still selects
hardware GLES2. In automatic mode, an early Vulkan compositor failure removes
the Vulkan layer and acknowledgement before the existing one-time GLES2
retry. This metadata bridge does not claim Vulkan rendering, dma-buf import,
modifier support, direct scanout, or display success; those remain live runtime
decisions.

Host fake-callback coverage is in `scripts/test-vulkan-drm-identity.sh`. It
checks exact opt-in and owner PID, every rejection gate, Vulkan
count/`VK_INCOMPLETE` semantics, dynamic rdev parsing, core and KHR properties,
physical-device-group dispatch, native-extension pass-through, bounded table
exhaustion cleanup and reuse, loader dispatch, and the versioned layer ABI.

The 2026-08-27 reference-device run measured `card0=226:0`,
`renderD128=226:128`, and dynamically allocated `kgsl-3d0=462:0`. Unmodified
Sway/wlroots stayed on `WLR_RENDERER=vulkan`; fullscreen 1920×1080 Mailbox
vkmark scored **12,660**, compared with **4,078** on the preceding GLES2 path.
After the lifecycle and child-scope hardening, the same device remained on
Vulkan and two bounded fullscreen Mailbox vertex runs measured **19,927–20,726
FPS**.
Sway's child processes inherited no open leased DRM descriptor; their inherited
owner value also differed from their own PID, so the layer stayed pass-through.

## Firefox preload regression and v12 correction

A later Vulkan-priority failure was not an identity-layer or Turnip probe
failure. The codec setup had appended two Firefox-only adapters to the entire
session `LD_PRELOAD`. The earlier EGL adapter interposed `dlsym`; when the DRM
preload asked for the next `drmModeGetPropertyBlob` implementation, that lookup
could resolve back to the DRM wrapper itself. The compositor then terminated
with `SIGSEGV` and the normal one-time recovery selected GLES2, which made the
failure look like a Vulkan-selection problem.

Runtime v12 applies two independent corrections:

1. Firefox adapters are removed from the compositor environment and are
   injected only by the verified `lindex-firefox` launcher.
2. The DRM preload opens the actual `libdrm.so.2` link-map object, resolves both
   property-blob functions from that object, verifies their owner with
   `dladdr`, and fails closed on a self/interposer result.

The host test suite now recreates the earlier `dlsym` interposer topology. On
the reference device, the production auto-video/Vulkan-priority configuration
kept the codec gateway active and unmodified Sway/wlroots on
`WLR_RENDERER=vulkan` for a bounded 117-second session, with no Firefox adapter
in compositor `LD_PRELOAD`, no GLES2 retry, and no teardown orphan. Browser
decode acceptance remains a separate pending gate.

## Performance follow-up

A later same-device regression check did not find a compositor or bridge binary
change behind the lower numbers. The installed wlroots 0.18.2 library matched
the Debian 0.18.2-3 file exactly, and the then-active v11 identity layer, DRM preload,
bridge core, vkmark 2025.01 device identity, and Turnip driver identity matched
the high-run path. The active framebuffer also remained exact XB24/QCOM
compressed rather than falling back to LINEAR.

One diagnostic runner had incorrectly omitted `FD_KGSL_ENABLE_DMABUF=1` from a
manually reconstructed LXQt client environment. Restoring the normal desktop
client environment raised its bounded fullscreen vertex result from 9,276 to
13,625 FPS. Replaying the historical `swaymsg exec` command on the current Sway
session produced 11,824 FPS; stopping Waybar produced 11,480 FPS, so panel
composition was not the regression source.

`FD_KGSL_ENABLE_DMABUF=1` is restored as a persistent runtime contract rather
than a benchmark-only override. Both `bin/stock-profile-session` and the
installed Debian `/etc/profile.d/99-android-kgsl.sh` payload export it, and the
source and packaged-module checks now reject either omission.

The remaining comparison was not power-state equivalent. During the follow-up,
KGSL exposed a hard `max_freq` of 607 MHz and `thermal_pwrlevel=8`; the battery
was 10% and the Android skin thermal status was light. Earlier saved runs exposed
1.2 GHz with `thermal_pwrlevel=0` and recorded substantial 734 MHz to 1.2 GHz
residency. The 19,927–20,726 FPS measurements are therefore retained as real,
but a charged, cooled run with the same 1.2 GHz ceiling is required before
claiming a renderer-path regression. LinDeX does not override kernel, OEM,
battery, or thermal GPU limits.

### Measured clock-state comparison

The numbers below keep vkmark's full-suite score separate from the bounded
vertex FPS metric. The 7,894-point LXQt run is deliberately excluded from the
607 MHz comparison because its reconstructed client environment omitted
`FD_KGSL_ENABLE_DMABUF=1`.

| Metric and path | 1.2 GHz, `thermal_pwrlevel=0` | 607 MHz, `thermal_pwrlevel=8` | Result |
|---|---:|---:|---|
| Full 1920x1080 Mailbox vkmark score, unmodified Sway/wlroots Vulkan | 12,660 | Pending an exact-environment rerun | High-clock reference score only |
| Bounded fullscreen Mailbox vertex FPS, historical Sway command | 19,927-20,726 | 11,824 | Low-clock run retained 57.0-59.3%; the high-clock path was 1.68-1.75x faster |
| Bounded LXQt diagnostic vertex FPS after dma-buf export restoration | Not a paired 1.2 GHz run | 13,625 | Diagnostic confirmation, not a cross-compositor score claim |

Turning the Android display on changed Power HAL from non-interactive to
interactive but left both `max_freq=607 MHz` and `thermal_pwrlevel=8` unchanged.
The measured reduction was therefore a thermal/power ceiling, not a screen-off
or wake-lock effect and not evidence of a Vulkan, wlroots, UBWC, or identity
bridge regression.
