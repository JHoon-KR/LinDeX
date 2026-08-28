# LinDeX

[English](README.md) | [한국어](README.ko.md)

> **Pre-release status:** LinDeX v3 is being prepared for its first public
> release. Host-side checks and packaged-module application encode tests have
> passed. The complete three-profile device matrix and decoded DRM PRIME gates
> are still pending. See [Validation status](docs/VALIDATION_STATUS.md).

LinDeX runs an ARM64 Debian Wayland desktop on a physical USB-C DisplayPort
output while Android keeps ownership of the phone display. It targets Qualcomm
Adreno/KGSL devices with MSM/SDE display hardware and is maintained by
**JHoon**.

LinDeX began from [DOAN](https://github.com/av2xn/DOAN) and
[Magisk-Debian-Chroot](https://github.com/av2xn/Magisk-Debian-Chroot), both by
**av2xn**. Their MIT notices are preserved in [LICENSE](LICENSE) and
[NOTICE.md](NOTICE.md). LinDeX is an independent continuation and claims no
affiliation with those upstream projects, device vendors, Android, or desktop
projects.

## What v3 provides

- A small DRM lease bridge for unmodified Wayland compositors. LinDeX does not
  require a compositor patch or install a compositor-specific display fork.
- Three selectable profiles: Archcraft Sway Free, LXQt + stock labwc, and
  XFCE + stock labwc.
- A pinned, checksum-verified copy of Archcraft's official public Sway
  dotfiles and wallpapers, plus public GPL-3.0 Dark and Light GTK, icon, and
  cursor assets. LinDeX reproducibly aggregates the appearance files from
  their documented public sources; the aggregate is not an upstream Archcraft
  distribution archive. No Ko-fi download or user-supplied archive is needed.
- A KernelSU module WebUI for profile setup, DP start/stop, display mode,
  direct-scanout compatibility, USB input routing, and video acceleration.
- Exact session/process-group ownership and lease cleanup on stop, failure, or
  physical cable removal.
- Separate `release` and `dev` packages. Release mode keeps no persistent
  setup/session/codec logs; dev mode retains bounded diagnostics.
- No files or overlay are installed under Android `/system`, `/vendor`, or
  `/product`.

## Native GPU acceleration

“Native” in LinDeX means that Debian applications and the Wayland compositor
submit work to the phone's physical Adreno GPU through Mesa. It does not mean
software rasterization, a streamed Android desktop, or an Android framebuffer
copied by the CPU. It also does not mean that every window is always placed on
a display plane without composition.

```text
Debian Wayland application
  |-- Vulkan -> Mesa Turnip --------------------|
  `-- OpenGL/GLES -> Mesa Freedreno `kgsl` -----|-> /dev/kgsl-3d0
                                                  -> dma-buf + native fence
                                                  -> stock Sway/labwc

Android display owner -> bounded DRM lease -> /dev/dri/card0 (MSM/SDE KMS)
                                               -> external DP plane/connector
```

These are two cooperating kernel interfaces, not two interchangeable GPU
drivers:

- `/dev/kgsl-3d0` is Qualcomm Android's stock kernel command-submission path
  for Adreno. Turnip uses it for Vulkan; the pinned Mesa fork's
  Freedreno/Gallium `kgsl` path uses it for OpenGL and GLES.
- `/dev/dri/card0` and `/dev/dri/renderD128` belong to the MSM/SDE DRM display
  side. LinDeX leases the external connector/CRTC/plane from `card0` and uses
  the DRM identity for allocation and display matching; it does not pretend
  that this node replaces KGSL as the Adreno renderer.
- Root access is needed to create the chroot, grant bounded device access, and
  broker the DRM lease. Root does not reveal a separate, faster universal GPU
  API. On this Android kernel, replacing `kgsl` with upstream Mesa's ordinary
  DRM `msm` loader would be valid only if the kernel exposed the corresponding
  conventional Adreno DRM render node and ABI.

The release compositors remain unmodified Debian packages: Sway 1.10.1 with
wlroots 0.18.2 for Archcraft Sway, and stock labwc for LXQt and XFCE. “Unmodified”
does not mean that no adapter is used. A process-scoped DRM lease bridge gives
only the owned compositor its already-authorized KMS descriptor, and a
fail-closed Vulkan explicit layer supplies missing DRM identity metadata only
for the exact live Qualcomm/Turnip/device tuple. No wlroots, Sway, labwc, Mesa,
kernel, or read-only Android partition is patched by this display path.

### Renderer selection and fallback

At session start LinDeX probes the installed ICD with `vulkaninfo`, rejects
software renderers such as lavapipe, and selects the hardware Turnip Vulkan
renderer when the exact device-identity gate passes. `FD_KGSL_ENABLE_DMABUF=1`
keeps Mesa's KGSL dma-buf export path active. If hardware Vulkan is unavailable
in automatic mode, LinDeX starts the hardware Freedreno/GLES2 path instead;
software rendering is not accepted as a successful fallback. A forced Vulkan
choice fails closed instead of silently changing renderer.

### dma-buf, UBWC, composition, and direct scanout

Rendered images cross from KGSL to MSM/SDE as dma-bufs with explicit format,
modifier, and fence metadata. `XB24`/`XR24` describe desktop pixel formats;
QCOM/UBWC and LINEAR describe their memory layout. They are distinct from the
NV12 video format discussed in the codec documentation.

New installs use the exact-gated **UBWC preference** policy. It retains LINEAR
fallback and changes nothing when the connected plane and live device identity
do not match the validated QCOM candidate. An opaque fullscreen client can be
scanned out directly as XB24/QCOM when its buffer and the leased primary plane
agree. A visible Waybar, notification, popup, transformed surface, color
operation, or software cursor can make the scene ineligible; stock wlroots then
correctly composites the desktop as XR24 LINEAR. LinDeX does not force direct
scanout by bypassing the compositor's overlap, transform, cursor, color, or
synchronization checks.

The three WebUI policies therefore have deliberately different meanings:

| Policy | Behavior |
|---|---|
| **UBWC preference** | Prefer the exact validated XB24/QCOM candidate for eligible fullscreen scanout, while retaining LINEAR fallback |
| **Auto** | Append the validated candidate without changing stock wlroots' format preference; compatibility-first and commonly composited LINEAR |
| **LINEAR** | Disable modifier use for compatibility |

### Reference-device evidence and claim boundary

The following figures are evidence from one Galaxy S25 Edge/Adreno 830 and are
not portable performance promises:

| Check | Measured result |
|---|---|
| Unmodified Sway/wlroots renderer | Hardware `WLR_RENDERER=vulkan`, Turnip on KGSL; no llvmpipe/lavapipe |
| Fullscreen 1920x1080 Mailbox vkmark | 12,660 Vulkan versus 4,078 on the preceding GLES2 path |
| Bounded fullscreen vertex workload | 19,927-20,726 FPS with a 1.2 GHz GPU ceiling; 11,824 FPS with a 607 MHz thermal ceiling |
| Current v3.0.3-dev full-suite snapshot | Score 11,013; sampled GPU load averaged 88.8% and peaked at 99%, with thermal constraint present in 89/120 samples |
| Same-lease direct scanout | Strict preference produced 88/91 XB24/QCOM active-framebuffer samples; the three LINEAR samples were entry/exit transitions |
| Composited control | With Waybar mapped, the active framebuffer was XR24 LINEAR as expected |

vkmark score and individual scene FPS are different metrics. Present mode,
scene, panel visibility, resolution, GPU clock ceiling, battery state, and OEM
thermal policy must match before two runs are compared. LinDeX does not force
GPU clocks or override Android thermal policy.

Display mode defaults to the connected sink's EDID mode. Manual 100, 120, and
144 Hz entries are experimental choices with no automatic rollback, and the
actual accepted refresh rate remains subject to the phone, adapter, cable, and
monitor. The current reference acceptance session used 1920x1080 at 60 Hz.

The reference-device Vulkan, UBWC-preference, and ordinary fullscreen
direct-scanout paths are implemented and boundedly verified. This is not a
claim of universal or “100%” acceleration: final tagged-package lifecycle
testing, reliable recovery after a forced physical DP loss, Android internal-
display competition checks, the EGL-only device fallback, and additional
Qualcomm devices remain release/portability gates. An optional, deterministic
Sway IPC state watcher that hides and restores Waybar for fullscreen scanout is
also not yet accepted; an always-visible bar intentionally remains composited.

The same runtime is shipped in both package flavors. `release` keeps WebUI
state but creates no persistent setup/session/codec diagnostic log; `dev`
retains only bounded, rotated diagnostics for reproducing a failure. See
[Architecture](docs/ARCHITECTURE.md),
[Vulkan DRM identity](docs/VULKAN_DRM_IDENTITY.md), and
[Validation status](docs/VALIDATION_STATUS.md).

## Install

1. Back up files stored inside an existing LinDeX Debian chroot.
2. Physically disconnect the DP cable or USB-C display adapter.
3. Install the rootfs-inclusive LinDeX ZIP with the KernelSU module installer.
4. Reboot Android normally so the root module is activated.
5. Open the LinDeX WebUI and choose a profile. For Archcraft Sway Free, choose
   **Official Dark**, **Official Light**, or **Auto-generated · Pywal**; Dark is
   the default and Pywal derives colors from bundled or user wallpapers.
6. Press **Start now**.
7. Connect DP when prompted, or enable automatic DP start after setup succeeds.

A fresh install extracts Debian 13 (Trixie) to `/data/local/debian`. Removing
the module also removes that chroot and its data. Read the full
[installation guide](docs/INSTALLATION.md) before installing or removing it.

## Profiles and attribution

Archcraft Sway Free is built from the official public
[`archcraft-os/archcraft-sway`](https://github.com/archcraft-os/archcraft-sway)
repository at commit `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`
(GPL-3.0). Its Dark and Light public theme, icon, and cursor sources are
separately pinned and attributed in [Profiles](docs/LINDEX_PROFILES.md).
LinDeX applies a small Debian/chroot safety adapter: Android-owned power
actions, internal brightness and Bluetooth bindings, and automatic lock/DPMS
are disabled; visual files remain the pinned public files. LXQt and XFCE use
signed Debian packages with stock labwc.

No PolicyKit agent is added by LinDeX: the root chroot has no usable logind
user session, so `lxpolkit`, `xfce-polkit`, and `lxqt-policykit` would only
display a misleading `No session for PID` failure. The LXQt profile installs
explicit components instead of the `lxqt-core` metapackage, retaining
`pcmanfm-qt` desktop/file management without pulling its PolicyKit agent.
Wayfire, River, and Newm are not release profiles because no matching official
public Archcraft dotfile set is packaged for them. See [Profiles](docs/LINDEX_PROFILES.md) and
[third-party notices](THIRD_PARTY.md).

Thanks to the Archcraft Sway creator. Their
[Ko-fi page](https://ko-fi.com/s/10f2e87af3) is an optional support link only;
it is not an installation source, required payment, or license condition.

## Current validation summary

| Area | Current public claim | Release gate |
|---|---|---|
| Host builds and unit/integration tests | Passing for the current source candidate | Re-run on the tagged commit |
| DRM lease and unplug cleanup | Focused reference-device and fixture validation completed | Final packaged-module reconnect matrix pending |
| Three profiles | Package/asset contracts and common dependencies are verified | Final start/stop/hotplug device matrix pending |
| Encode | H.264 CB and HEVC Main each completed 120/120 720p60 frames on QTI hardware at 84.9 and 81.1 fps. PRIME/OBS input uses zero CPU raw-pixel copies and one GPU conversion; generic FFmpeg upload uses one CPU copy | The Vulkan producer optimization is host/NDK validated; repeat the measurements from the final package |
| Decode byte path | Reference-device PASS: 720x360 NV12, stride 768, slice height 384, 436,176 bytes, one raw-frame CPU copy | Repeat against the exact release artifact |
| Decode QCOM UBWC PRIME | H.264 Main+B 180/180, HEVC Main+B 120/120, and VP9 Profile 0 120/120 passed PRIME, release, EOS, and codec stop. H.264 uses the exact bounded reorder gate | HEVC Main and VP9 Profile 0 are public-beta advertised profiles after their live preflight; displayed playback and seek remain app-specific tests. H.264 High, HEVC Main10, and AV1 stay hidden |
| Decode modifier `0` LINEAR | The same long runs passed explicit LINEAR output with matching content, one Vulkan repack, zero CPU raw-pixel copies, and measured repack averages of 1.38–1.74 ms | Repeat displayed playback, seek, EOS, and teardown through each packaged application |
| Firefox RDD sandbox gateway | Installed v3.0.3-dev passed 30 seeks and 10 document reloads: seccomp mode 2, RDD KGSL FD 0, `c2.qti.avc.decoder`, 766 decoded/5 dropped frames, 1,757 hardware outputs, 1,758 LINEAR exports, no timeout/async failure, and clean session teardown/restart | Repeat from the tagged public release ZIP and complete exact per-frame gateway fence accounting |
| Direct scanout | PASS on the pristine Debian wlroots 0.18.2/Sway 1.10.1 stack: strict UBWC preference produced 88/91 exact XB24/QCOM active-FB samples; three LINEAR samples were entry/exit transitions and the desktop returned to XR24 LINEAR when Waybar was restored | Compatibility-first `auto` only appends the candidate and remained XR24 LINEAR in 90/90 samples; composited UBWC still needs a versioned wlroots adapter or patch |

“Zero CPU copy” does not mean zero GPU work. The validated encode candidate
uses one bounded GPU blit. Decode capabilities remain individually fail-closed:
the validated H.264 QCOM path may be exposed only by the exact artifact gate.
HEVC Main and VP9 Profile 0 are advertised as public beta only when their own
live QTI/PRIME/120-of-120 preflight passes. Application display and seek remain
compatibility evidence, not prerequisites that callers can forge. Modifier
value `0` is accepted only as explicitly declared DRM LINEAR metadata—not as
missing metadata. Details are in
[Codec and zero-copy status](docs/VIDEO_ZERO_COPY_STATUS.md).
The application-facing `auto|linear|qcom` rules and migration from the legacy
selector are documented in
[VA-API modifier policy](docs/VAAPI_MODIFIER_POLICY.md).

## Safety boundaries

- Detach DP before installing or updating.
- Reboot Android normally after installing or updating the module.
- Never remount or modify Android read-only partitions for LinDeX.
- Strict zero-copy mode never silently falls back to a CPU raw-pixel copy.
- Manual 100/120/144 Hz modes are experimental and have no automatic rollback.
- Back up the chroot before uninstalling.

See [Safety and recovery](docs/SAFETY.md) and
[Troubleshooting](docs/TROUBLESHOOTING.md).

## Documentation

The [documentation index](docs/README.md) links the paired English and Korean
user, architecture, profile, codec, release, and contributor guides. Source
protocol references and release-engineering notes are listed separately from
end-user documentation.

## Source and releases

The repository contains source, module files, verification scripts, public
documentation, and the checksum-locked official public Archcraft Sway assets
required by that profile. It intentionally excludes rootfs archives,
downloaded Mesa, retired provider archives, build outputs, device logs,
credentials, Ko-fi/private assets, and historical compositor experiments.
Published module ZIPs are rootfs-inclusive release artifacts built from
reviewed inputs.

See [Release packaging](docs/RELEASING.md), [Contributing](CONTRIBUTING.md), and
[Security](SECURITY.md).

## License

LinDeX code is released under the MIT License. Bundled or downloaded third-party
components keep their upstream licenses. See [LICENSE](LICENSE),
[NOTICE.md](NOTICE.md), [THIRD_PARTY.md](THIRD_PARTY.md), and
[License and attribution](docs/LICENSE_AND_ATTRIBUTION.md).
