# LinDeX architecture

[English](ARCHITECTURE.md) | [한국어](ko/ARCHITECTURE.md)

## System boundary

LinDeX separates Android display ownership, Debian rendering, and physical DP
scanout. The module does not remount or write Android `/system`, `/vendor`, or
`/product`. Persistent module state lives below
`/data/adb/debian-drm-lease-kit`; the Debian chroot lives at
`/data/local/debian`.

```text
Android display owner
  -> bounded DRM lease broker
  -> inherited lease file descriptor
  -> Debian libseat/DRM bridge
  -> unmodified Wayland compositor
  -> MSM/SDE physical DisplayPort output
```

Qualcomm Android devices expose rendering through KGSL (`/dev/kgsl-3d0`) and
display scanout through MSM/SDE DRM (`/dev/dri/card0`). LinDeX keeps those roles
separate and does not pretend that KGSL is a conventional DRM render node.

## Native renderer pipeline

The renderer and the display controller have separate jobs:

| Layer | Vulkan path | OpenGL/GLES fallback | Display role |
|---|---|---|---|
| Debian userspace | Mesa Turnip | Mesa Freedreno/Gallium `kgsl` | Unmodified Sway/wlroots or labwc owns allocation, composition and scene policy |
| Android stock kernel ABI | Adreno command submission through `/dev/kgsl-3d0` | Adreno command submission through `/dev/kgsl-3d0` | MSM/SDE KMS is exposed through `/dev/dri/card0`; `/dev/dri/renderD128` participates in DRM allocation/identity matching |
| Cross-device handoff | dma-buf object, explicit DRM format/modifier and native fence | dma-buf object, explicit DRM format/modifier and native fence | Leased connector, CRTC and primary plane drive the physical DP sink |

KGSL is the normal Qualcomm GPU submission ABI on the supported Android stock
kernel, including on a rooted device. Root permits bounded access to that ABI
and to the leased display resources; it does not create a faster hidden render
node. An ordinary upstream Mesa `msm` path would require a kernel that actually
exports the corresponding conventional Adreno DRM render-node ABI. Selecting
it merely because `/dev/dri` exists would confuse the MSM/SDE display side with
the KGSL renderer and is not a valid optimization.

The stock session starts with a clean renderer environment and then performs a
live hardware probe. It accepts a non-software Freedreno/Turnip ICD, enables
`FD_KGSL_ENABLE_DMABUF=1`, and selects `WLR_RENDERER=vulkan` only when the exact
Vulkan/DRM identity gate also passes. In automatic mode, an unavailable or
early-failing Vulkan path receives one bounded retry with hardware
`WLR_RENDERER=gles2` and `MESA_LOADER_DRIVER_OVERRIDE=kgsl`. A forced Vulkan
policy fails closed; llvmpipe, lavapipe, SwiftShader and other software
renderers do not satisfy the hardware probe.

This is native GPU acceleration but not an all-direct-scanout claim. KGSL can
render every composited frame on the GPU while MSM/SDE scans out the
compositor's LINEAR swapchain. Direct scanout is the narrower case in which one
eligible client dma-buf can be placed directly on the leased primary plane.
UBWC is likewise a memory-layout modifier, not a different pixel format or a
different GPU API: desktop `XB24`/`XR24`, video `NV12`, and QCOM/UBWC versus
LINEAR describe different parts of the buffer contract.

## Display bridge

The bridge is loaded only into the owned compositor session. It returns an
already-authorized lease descriptor when that session opens the matching KMS
device. Other device opens continue normally. The compositor keeps control of
its renderer, allocator, scene graph, and direct-scanout decisions.

The default policy preserves the compositor's modifier negotiation. A narrow
frontend can append only the exact lease-validated QCOM candidate to the
leased primary plane's `IN_FORMATS` data while keeping existing formats and
DRM LINEAR available. It does not override coverage, overlap, transform,
color-management, cursor, or synchronization checks. The WebUI offers `auto`
and a compatibility `off` mode for direct scanout; it has no unsafe force-on
mode.

New installs select the exact-gated UBWC preference by default; failed device
validation leaves the original KMS data untouched. The two `auto` settings
have different scopes. Output-modifier `auto` is a
compatibility-first candidate append; it does not express a format preference.
Direct-scanout `auto` leaves scene eligibility to wlroots. On the reference
device, pristine wlroots 0.18.2 initializes its composited output format to
XR24. Removing Waybar and running an opaque fullscreen vkmark surface still
produced 90/90 XR24 LINEAR samples with output-modifier `auto`. With the exact
strict UBWC preference enabled, the same test automatically transitioned from
LINEAR into 88 XB24/QCOM samples and back to LINEAR when Waybar returned. Thus
direct scanout is automatic once the client and plane agree on XB24/QCOM, but
candidate exposure alone does not make stock wlroots prefer it. Composited
desktop UBWC requires a versioned wlroots ABI adapter or a source patch because
the choice lives above libdrm's `IN_FORMATS` data.

Waybar does not disable modifier negotiation. With the Archcraft bar mapped on
the top layer, the output scene contains the fullscreen client and a second
layer-shell buffer. Stock wlroots 0.18 and 0.20 only enter their direct-scanout
path for a single eligible scene buffer, so they skip the scanout test and use
Sway's XR24 LINEAR compositor swapchain instead. Notifications, launchers,
popups, and a software cursor can cause the same temporary fallback. Removing
the extra scene entry returns to XB24/QCOM automatically; forcing scanout while
the bar remains visible would simply omit the bar and is not supported.

A future optional fullscreen-performance mode must use deterministic Sway bar
IPC rather than Waybar 0.12's `SIGUSR1` toggle. Sway should own one named bar,
Waybar should subscribe with the same ID and `ipc: true`, and a bounded Sway
event watcher should set that bar to `invisible` only for a true fullscreen
container and restore `dock` afterward. The watcher must restore state after an
IPC reconnect and must not claim success until same-lease GETFB2 A/B evidence
shows the expected LINEAR-to-QCOM transition. An always-visible composited bar
remains LINEAR unless a separately version-gated compositor adapter is added.

Every bridge and preload-only path is bounded by live identity and capability
checks and fails closed when a gate is missing. A successful lease or modifier
bridge does not by itself prove direct scanout; the unmodified Wayland
compositor remains responsible for its complete scene and scanout decision.

LinDeX uses a pinned KGSL-capable Mesa build. Vulkan is preferred only when its
live probe succeeds; GLES remains a fallback. A direct-scanout result still
requires the application buffer, renderer, and leased plane to agree on format
and modifier.

On the exact validated Turnip stack, a standard explicit-layer
[Vulkan DRM identity bridge](VULKAN_DRM_IDENTITY.md) supplies the missing
`VK_EXT_physical_device_drm` metadata that unmodified wlroots needs to match the
KGSL renderer to MSM/SDE KMS. Its exact driver, capability, node and
runtime-derived rdev gates
are independent of the DRM `IN_FORMATS` modifier bridge.

Preload scope is also a renderer-safety boundary. The compositor receives only
the display, seat, and optional USB-input bridge libraries. Firefox-specific
EGL/RDD adapters are injected by the packaged browser launcher and never enter
Sway or labwc. Runtime v12 additionally resolves the two property-blob entry
points from the loaded `libdrm.so.2` object and rejects a self/interposer result.
This prevents an unrelated `dlsym` adapter from turning the DRM hook into
recursion, a compositor crash, and a misleading GLES fallback.

## Session lifecycle

Every launch receives a unique session identifier, leader PID, process group,
per-session ownership token, and lease. Cleanup scans the complete token-owned
group, so a launcher leader that exits first cannot leave a compositor child
holding a DRM fd. Normal stop, compositor failure, and physical DP removal
terminate only that owned group and close its descriptors. An explicit
connector `disconnected` state stops on the first watcher sample; only a still
connected link with a transiently unreadable EDID uses the two-sample debounce.
Reconnect must provide valid DP+EDID for two consecutive samples and creates a
new lease, token, and process group; a failed lease is never reused.

Automatic start requires both a physically connected DP connector and valid
EDID data. The selected monitor mode is taken from the connected sink unless
the user explicitly selects an experimental manual mode.

## Debian and profile setup

The Android module owns WebUI commands, rootfs extraction, device discovery,
session control, and optional MediaCodec services. The Debian chroot owns
compositor packages, user configuration, Mesa, and profile runtimes.

All three release profiles share common base dependencies. LinDeX does not add
`lxpolkit`, `xfce-polkit`, or `lxqt-policykit`, because the root chroot has no
usable logind user session for an authentication agent. LXQt is therefore
installed as explicit components instead of through `lxqt-core`. Archcraft
Sway Free installs pinned,
checksum-verified official public GPL-3.0 Sway dotfiles and wallpapers plus
Dark/Light GTK, icon, and cursor assets from the documented public commits.
The appearance archive is a LinDeX-built reproducible aggregate, not an
upstream Archcraft distribution. Before launch, the profile manager applies
Dark, Light, or the official `~/.config/sway/theme/theme.sh --pywal` path. A
narrow Debian/chroot adapter disables Android-owned power, internal-brightness
and Bluetooth actions plus automatic lock/DPMS. LXQt and XFCE use stock labwc.
Wayfire, River, Newm, private provider stacks, and Ko-fi archives are outside
the current release boundary.

## Video bridge

The optional ADVC service is independent of the display lease. Backend
selection is made once per codec session:

```text
Vulkan dma-buf -> EGL dma-buf -> Android-local AHB/Surface -> byte transport
```

Strict zero-copy mode permits only a live-verified dma-buf backend and never
falls back to a CPU raw-pixel copy. Auto mode may use Android-local hardware or
byte transport. A backend failure tears down owned buffers, descriptors, and
fences before a new session is created; it does not switch mid-frame.

The encode candidate has demonstrated zero CPU raw-pixel copies with one
bounded GPU blit into a MediaCodec Surface on the reference device. Decode
long-runs completed H.264 Main+B 180/180, HEVC Main+B 120/120, and VP9 Profile
0 120/120 through QCOM/UBWC-to-explicit-LINEAR Vulkan repack with zero CPU raw-
pixel copies. The installed v3.0.3-dev Firefox RDD route also completed 30
seeks and 10 document reloads through `c2.qti.avc.decoder`, with the sandbox
enabled and no KGSL descriptor in RDD. HEVC Main and VP9 Profile 0 are now
public-beta advertised profiles after their exact live preflights; displayed
application compatibility and the public tagged-release repetition remain to
be expanded. A modifier value of `0`
is valid only when the descriptor explicitly declares DRM LINEAR; it is not
treated as absent metadata.

See [Codec and zero-copy status](VIDEO_ZERO_COPY_STATUS.md) for the claim
boundary and pending release gate.

## Input routing and logging

USB keyboard, mouse, and indirect touchpad ownership can be scoped to the
active compositor session. The phone touchscreen and unrelated USB functions
remain with Android. Closing the last owned descriptor releases the grab.

Release packages route setup, session, and codec diagnostic streams to
`/dev/null` and remove stale persistent logs. Dev packages retain only bounded,
rotated diagnostics. See [USB input routing](USB_INPUT_ROUTING.md) and
[Safety](SAFETY.md).
