# Codec discovery and VA-API profile policy

[English](CODEC_DISCOVERY_AND_VAAPI_PROFILES.md) | [한국어](ko/CODEC_DISCOVERY_AND_VAAPI_PROFILES.md)

## Android component discovery

The Android capability provider asks MediaCodec for a decoder by exact MIME,
not by a fixed component name. It probes `video/avc`, `video/hevc`,
`video/x-vnd.on2.vp9`, and `video/av01`, so QTI, MediaTek, Exynos, Rockchip, or
another device may return different component names without changing codec
selection.

The returned name is only fail-closed hardware evidence. Known Android and
FFmpeg software prefixes are classified as software. Secure, software-marked,
embedded-prefix, and unknown names remain `UNKNOWN`; they are not promoted to
hardware. This can produce a safe false negative on a new vendor. Adding a new
hardware prefix requires device evidence and a host test. A component name
alone is never proof that PRIME export, access-unit translation, EOS, or an
inter-frame stream works.

The NDK r27 media API exposes MIME-based creation and the selected component
name, but no public MediaCodecList/profile-level enumeration API. Therefore a
successful MIME lookup is deliberately not converted into a device profile
claim; the profile gates below still require bounded live evidence.

## VA-API advertisement

`vainfo`, FFmpeg, and GStreamer see the intersection of all of these gates:

1. the broker reports decode and PRIME transport;
2. MediaCodec selected a non-secure component classified as hardware for the
   MIME;
3. the VA access-unit translator implements the exact profile; and
4. the exact per-codec live-validation environment gate is enabled.

The only possible standard VA decode profiles are H.264 Constrained Baseline
and Main, HEVC Main, and VP9 Profile 0. H.264 Baseline/High, HEVC Main10 and
other HEVC profiles, VP9 Profiles 1-3, all unknown profiles, and every AV1
profile fail closed in profile enumeration, entrypoint queries, attribute
queries, and config creation.

AV1 remains intentionally hidden even if Android reports a hardware AV1
decoder. The current standard VA-API AV1 buffers do not provide enough
authoritative sequence-header state to construct the MediaCodec input stream
without guessing. MIME discovery must not hide that semantic gap.

## Remaining live gates

Host tests prove exact MIME/name classification and the VA profile allowlist;
they do not prove a device. Before enabling a profile on another device, record
the selected component and pass a bounded decode using that profile with PRIME
export, an inter-frame sequence, explicit EOS/drain, surface release, and
repeated teardown. Unknown component names stay disabled until that evidence
exists. No stock-application support claim follows from discovery alone.
