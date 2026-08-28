# VA-API patch order

Apply these patches from the LinDeX repository root in numeric order:

1. `0001-gstreamer-query-surface-drain.patch`
2. `0002-ffmpeg-multi-slice-layout.patch`
3. `0003-private-decode-eos-contract.patch`
4. `0004-gstreamer-h264-decode-diagnostic.patch`

These are patches for the LinDeX source tree, not a patch series applied to a
system GStreamer installation. The release module contains none of these
`.patch` files. The first patch changes surface-status polling and its live probe. The second
patch changes slice-buffer staging and adds an independent layout helper and
unit test. The third adds an exact-opt-in private EOS contract; it does not make
stock FFmpeg or GStreamer EOS-aware. Despite its filename, the fourth only
adds LinDeX diagnostic scripts, fixtures and documentation. It does not patch
GStreamer code or change the release runtime. Apply them in this order.

Verification:

```sh
git apply --check patches/vaapi/0001-gstreamer-query-surface-drain.patch
git apply patches/vaapi/0001-gstreamer-query-surface-drain.patch
git apply --check patches/vaapi/0002-ffmpeg-multi-slice-layout.patch
git apply patches/vaapi/0002-ffmpeg-multi-slice-layout.patch
git apply --check patches/vaapi/0003-private-decode-eos-contract.patch
git apply patches/vaapi/0003-private-decode-eos-contract.patch
git apply --check patches/vaapi/0004-gstreamer-h264-decode-diagnostic.patch
git apply patches/vaapi/0004-gstreamer-h264-decode-diagnostic.patch
```
