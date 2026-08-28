# Decode failure analysis

This document separates resolved and remaining decode failures by ownership
boundary. A gate is enabled by the stock LinDeX session only after its device
acceptance passes; unresolved diagnostic gates remain disabled.

## 1. H.264 IDR-to-P transition

VA-API does not expose the PPS default active-reference counts in the H.264
picture parameter buffer. LinDeX can recover them only when the first P/B slice
that does not override the defaults is submitted. The old path regenerated the
same PPS id at that point and sent it as a new `CODEC_CONFIG` input before the
P-frame. A Qualcomm MediaCodec component may treat such a mid-stream config
buffer as a deferred reconfiguration boundary, which matches the observed
IDR success followed by P-frame latency or a tail stall.

The isolated target-device gate now passes four H.264 Main frames with four
slices per frame, PRIME export, ordered PTS, EOS, surface release and codec
stop. The broker queued no mid-stream `CODEC_CONFIG`; the in-band candidate was
therefore neither exercised nor needed for this fixture. It remains disabled
until a stream that actually changes PPS proves the gate below:

`ADVC_VAAPI_H264_INBAND_CONFIG_UPDATE=validated-pps-v1`

Required device verdict: an IDR-to-P stream must produce both frames, preserve
PTS ordering, reach terminal EOS and export the expected PRIME descriptor.

## 1a. H.264 Main B-frame reorder metadata

The initial SPS reconstruction omitted VUI entirely. That is sufficient for
the no-B-frame fixtures above, but it does not preserve the source
`max_num_reorder_frames` contract because VA-API does not expose the original
SPS. On the reference Qualcomm decoder, a 320x240 Main stream with two B frames
then accepted input without returning output and eventually filled the input
window. The same 320x240 and 1280x720 paths completed when B frames were zero,
which isolates the failure from resolution and PRIME allocation.

The fail-closed gate emits only the VUI bitstream-restriction subset; it does
not invent aspect, colour, timing, or HRD metadata. Isolated decoding initially
validated a four-frame bound, but the displayed Firefox submission window
required a one-frame bound:

`ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1`

The two-B-frame 320x240 stream completed 30/30 and the 1280x720 browser fixture
completed 180/180 at 16.6x with zero FFmpeg decode errors under the isolated
bound. On the real Firefox ESR/RDD path, bounds four and two stalled before the
first required export and zero damaged presentation ordering. Bound one
completed 30 seeks and 10 document reloads through the installed v3.0.3-dev
production gateway. Firefox reported 766 decoded and 5 dropped frames, while
the VA driver recorded 1,757 hardware outputs, 1,758 successful exports, no
export timeout, and no asynchronous failure. RDD remained in seccomp mode 2
with zero KGSL file descriptors, and Android selected `c2.qti.avc.decoder`.
Stopping the active session left no Firefox, gateway, Sway, or session-runner
orphan. The stock session now exports the validated one-frame gate; the public
tagged release ZIP still needs repetition and exact gateway fence accounting.

## 2. Application EOS contract

Stock VA-API has no generic "compressed input has ended" call for a vendor
driver. LinDeX therefore cannot infer EOS safely from `vaSyncSurface()`,
`vaQuerySurfaceStatus()` or context destruction. The private EOS ABI now has a
single dequeue owner and deterministic terminal-state handling, but it remains
an explicit opt-in contract.

The current private ABI 1.0 is not enough for a safe GStreamer finish hook. A
plugin needs the ready `VASurfaceID`, an idempotent per-surface release method
and release-fence ownership. Without those fields, progress can exhaust the
eight broker leases or publish frames out of order. The prerequisite GStreamer
patch therefore adds only the fail-closed release hook; it does not pretend to
finish stock application integration.

## 3. VASurface EOS and process exit

The old `memory:VASurface ! fakesink` run decoded every input and posted sink
EOS, while the process still timed out. The cause was not MediaCodec stop or an
undrained `AImageReader`. `advc_dmabuf_descriptor_close()` scanned all four
object slots even when `object_count` was zero. A zero-initialized unused slot
therefore called `close(0)`, destroying GStreamer's bus/poll wakeup FD. GstBin
had already decided that every sink posted EOS, but the bus callback could no
longer wake and the main loop spun on `POLLNVAL`.

Cleanup now closes only unique FDs declared by `object_count`; producers must
publish contiguous ownership before a later operation can fail. A regression
test proves that a zeroed descriptor cannot close FD 0 and that undeclared
slots remain untouched.

The exact old-driver/new-driver A/B test now completes the explicit H.264
`memory:VASurface ! fakesink` pipeline, observes downstream and bus EOS, exits
within the bound, discards zero codec outputs, keeps destroy drain disabled and
returns `AMediaCodec_stop()` status 0 for the same session. The destroy-drain
candidate was disproved and must remain disabled.

## 4. GStreamer registry scanning

Killing `gst-plugin-scanner` is not a codec-discovery fix. It can leave a
partial registry and cause the healthy VA plugin to be replayed as blacklisted.
LinDeX performs the ADVC preflight before `gst_init`, uses a session-owned
registry and sets `GST_REGISTRY_FORK=no`. Scanner processes must not be killed
globally.

## Device gates before release

- H.264: multi-slice plus IDR-to-P, frame/PTS/EOS/PRIME checks.
- HEVC: one-IRAP passes; a four-frame Main I/P/B inline-RPS stream passes
  transport, PRIME, EOS and stop. A validation-only hook maps surface IDs to
  POC, reads the exact PRIME allocation through Turnip, and byte-exactly
  matches all four NV12 crops against software decode after POC sorting.
- VP9: Profile 0 key plus inter passes PRIME, release, EOS and stop.
- GStreamer: the explicit old-VAAPI H.264 VASurface pipeline must report
  `gstreamer-h264-vasurface-eos-teardown-pass`; playbin/decodebin, seek, display
  and automatic decoder selection remain separate gates.
- Reviewed runtime code may be packaged so independently validated codecs can
  share one driver, but every unproven capability remains unadvertised until
  its own target-device gates pass.
