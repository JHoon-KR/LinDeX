# Firefox RDD decode gateway

LinDeX keeps the Firefox RDD seccomp sandbox enabled. The packaged Firefox
path does not grant KGSL ioctls to RDD and does not set
`MOZ_DISABLE_RDD_SANDBOX`.

## Runtime boundary

The release route is:

```text
Firefox RDD (sandbox enabled)
  -> exact preconnected SOCK_SEQPACKET socket
  -> LinDeX repack gateway (Turnip/KGSL owner)
  -> Android ADVC broker
  -> Android c2.qti decoder
  -> QCOM/UBWC NV12 AHB + source fence
  -> one Vulkan GPU repack in the gateway
  -> explicit modifier-0 LINEAR NV12 dma-buf + destination fence
  -> Firefox RDD
```

The RDD-facing VA driver is built without Vulkan or Turnip linkage. It accepts
the gateway's explicit LINEAR descriptor and cannot initialize KGSL through an
in-process repack hook. The two preload adapters are exact-process gates:

- the socket adapter activates only for a Firefox `rdd` content process, the
  exact `/run/android-drm/` gateway socket, and the versioned acknowledgement;
- the EGL identity adapter activates only for Firefox `glxtest`, the exact
  KGSL/DRM nodes, and the versioned acknowledgement.

All other processes receive native behavior.

The codec adapters are not part of the compositor-wide `LD_PRELOAD`. The
session exports their exact immutable paths through `LINDEX_FIREFOX_PRELOAD`,
and `/usr/local/bin/firefox` plus `firefox-esr` point to the packaged
`lindex-firefox` launcher. That launcher verifies both files are regular,
non-symlink artifacts before replacing the browser's preload set. This keeps a
Firefox `dlsym` interposer out of Sway/labwc and their Vulkan loader while
preserving the process gates above inside the browser tree.

## Gateway rules

The gateway accepts root peers only, uses bounded client and output tables,
and forwards the existing ADVC protocol. For decoded AHardwareBuffer output it
retrieves the broker-owned PRIME descriptor itself. It never trusts a
client-supplied QCOM layout.

- QCOM-compressed NV12 is repacked once by Turnip and exported as explicit
  modifier `0` LINEAR NV12.
- Already-LINEAR NV12 remains LINEAR and is not repacked.
- Unsupported formats fail closed; QCOM metadata is never relabeled as LINEAR.
- The source release fence is returned to Android and the destination acquire
  fence is returned to Firefox.
- The gateway has a parent-death signal, so compositor/session termination or
  forced DP teardown cannot leave it orphaned.

Encode byte and dma-buf operations continue through the same protocol. AHB
encode queue/transfer operations are deliberately not exposed through the
Firefox gateway.

## Release and development behavior

Release mode leaves gateway tracing disabled and does not accumulate a codec
log. Development mode may enable the exact `ANDROID_DRM_CODEC_TRACE=1` gate for
bounded diagnostics. The ordinary VA-API and GStreamer capability environment
is exported only after the immutable codec manifest, Android broker, gateway,
and socket have all passed their startup checks.

## Device acceptance gate

The packaged route remains fail-closed until the exact release ZIP passes all
of these checks on a supported device:

1. RDD reports seccomp mode 2 and has no `MOZ_DISABLE_RDD_SANDBOX` environment.
2. RDD owns no `/dev/kgsl-3d0` file descriptor and produces no KGSL seccomp
   violation.
3. Android selects the hardware `c2.qti` decoder.
4. Every decoded QCOM frame has one gateway repack, one source release fence,
   and one LINEAR destination acquire fence.
5. Firefox receives only explicit modifier-0 LINEAR descriptors, with no
   `EGL_BAD_MATCH` or software decode fallback.
6. Play, seek, drain, tab close, RDD exit, gateway exit, and forced session
   teardown leave no codec output, dma-buf, socket, or helper process orphan.

Host artifact, manifest, and package verification is complete. Reference-device
results must be recorded from the final installed ZIP before this candidate is
described as a completed browser hardware-decode path.

The 2026-08-28 installed v3.0.3-dev acceptance used the packaged ordinary
production socket, gateway, broker, and hardware decode route; the experimental
Android low-latency switch was not required. Firefox completed 30 seeks and 10
document reloads, reporting 766 decoded and 5 dropped frames. Driver tracing
recorded 1,757 hardware outputs, 1,758 successful LINEAR exports, zero export
timeouts, and zero asynchronous failures. Android media metrics named
`c2.qti.avc.decoder` and reported low-latency mode off. RDD stayed in seccomp
mode 2, mapped the packaged VA driver and both process-scoped adapters, and
owned zero KGSL file descriptors. Stopping the active session left no Firefox,
gateway, Sway, or session-runner orphan and the next Sway session started
normally. This closes the earlier first-B-frame/reorder stall and lifecycle
check for the installed development ZIP. The public release ZIP repeat and
exact per-frame gateway fence-count audit remain open acceptance items.

The 2026-08-27 staged-source integration check did confirm the new process
boundary: with video acceleration set to `auto`, the gateway remained active
while unmodified Sway/wlroots stayed on `WLR_RENDERER=vulkan` for a bounded
session and never entered its GLES2 retry. A direct `vainfo` probe reached
VA-API 1.22, loaded `advc_drv_video.so`, and found `__vaDriverInit_1_0`, but did
not complete its capability query. It was terminated without leaving a gateway
or session orphan. This is compositor-isolation evidence only; it does not
close the Firefox playback acceptance gate above.
