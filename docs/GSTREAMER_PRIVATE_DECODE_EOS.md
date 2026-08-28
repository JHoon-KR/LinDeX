# GStreamer private H.264 decode EOS integration

The operational `vaapih264dec` EOS patch is **blocked and must not be installed**.
The LinDeX release ZIP does not contain or install this upstream patch; all
current device results use Debian's unmodified GStreamer packages.
The checked-in GStreamer patch is only a dormant prerequisite: it exposes the
decoder's VA handles and adds a fail-closed callback before a surface returns
to its pool. Stock behavior is unchanged when the callback is unused.

The block is structural, not a retry tuning issue. Private decode-EOS ABI 1.0
can signal EOS and make at most one output progress, but its status does not
identify the `VASurfaceID` that became ready. It also cannot release a direct
broker output lease when downstream drops the final buffer. Upstream
gstreamer-vaapi 1.26.2 caches decoded VA surfaces in its pool, so buffer release
does not destroy the VA surface. At stream end there may be no next
`vaBeginPicture()` to release that lease. Guessing DPB/output order or draining
the eight-slot broker window would be unsafe.

The minimum unblock is a new private ABI that returns the exact ready surface
ID from `progress(1)` and provides a bounded, idempotent per-surface release
operation, including a defined release-fence contract. The future downstream
path must be H.264-only, resolve the versioned getter with `vaGetLibFunc()`,
take the original finish path if discovery/gating fails, signal before pushing
the software DPB tail, and interleave exactly one progress, one verified frame
push, and that frame's pre-pool release. Last-frame EOS and control EOS are
both valid. After private signaling begins, mismatch, error, output-window
pressure without a tracked releasable frame, or timeout must fail closed. A
known in-flight frame may wake one bounded `NEED_OUTPUT_RELEASE` retry when its
pre-pool callback releases the lease; this is not permission to block-drain.

The earlier independent teardown hypothesis was disproved. In the explicit
VASurface diagnostic, every sink posted EOS, but descriptor cleanup closed an
undeclared zero-valued object slot and therefore closed FD 0, which GStreamer
was using as its bus/poll wakeup descriptor. Restricting cleanup to the unique
FDs declared by `object_count` makes the pipeline post bus EOS and exit cleanly
with destroy drain disabled, zero discarded codec outputs and codec stop status
0. A bus-EOS process timeout is now neutrally classified as
`post-bus-eos-process-timeout`; it is not evidence of MediaCodec teardown.

This fix does not remove the private ABI limitation above. It proves only the
explicit old-VAAPI H.264 VASurface/fakesink EOS lifecycle, not playbin,
decodebin, seek, display or automatic decoder selection.

The complete proof, source hashes, patch, and verification procedure are in
[`patches/gstreamer/README.md`](../patches/gstreamer/README.md).

Verify against an untouched upstream 1.26.2 tree:

```sh
sh scripts/test-gstreamer-private-decode-eos-downstream.sh \
  /path/to/gstreamer-vaapi-1.26.2
```

Use `--full-build` only on a host with the required development dependencies.
The test builds in a temporary directory and never replaces a system plugin.
