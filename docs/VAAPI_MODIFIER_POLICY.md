# VA-API modifier policy

LinDeX defaults both directions to `auto`. `auto` is fail-closed: a QCOM
compressed dma-buf is used directly only when the descriptor carries the real
QCOM modifier, the broker advertises the corresponding probed capability, and
the exact importer-validation gate is present. Otherwise the safe output or
input route is an explicit modifier-0 LINEAR dma-buf. No route relabels UBWC as
LINEAR and no modifier fallback copies pixels on the CPU.

## Configuration and migration

| Direction | Variable | Values | Default |
|---|---|---|---|
| Decode output | `ADVC_VAAPI_DECODE_OUTPUT` | `auto`, `linear`, `qcom` | `auto` |
| Encode input | `ADVC_VAAPI_ENCODE_INPUT` | `auto`, `linear`, `qcom` | `auto` |

`ADVC_VAAPI_OUTPUT` remains a deprecated compatibility alias. A non-empty
direction-specific variable always wins. An unknown value is an initialization
error; it is not interpreted as `auto`.

`qcom` is a diagnostic forced mode. It fails with an unsupported error unless
all direct-import gates pass. It never falls back silently. `linear` accepts
only modifier `0` for an external encode input and always requests LINEAR for
decode output. Internally allocated writable encode surfaces remain LINEAR in
all modes, including forced `qcom`, because OBS and ordinary VA producers need
a writable, interoperable surface.

The validation tokens are intentionally separate from the selector:

- decode consumer: `ADVC_VAAPI_QCOM_IMPORT=validated-v1`;
- encode broker importer: `ADVC_VAAPI_ENCODE_QCOM_IMPORT=validated-v1` plus the
  broker capability produced by
  `ADVC_ENCODE_QCOM_IMPORT_VALIDATION=validated-turnip-qcom-nv12-surface-v1`;
- GPU fallback: `ADVC_VAAPI_GPU_LINEAR_REPACK=validated-qcom-nv12-v1`.

Selectors do not create a capability. They only choose among capabilities that
were independently proved.

## Data flow

| Operation and source | `auto` result | Forced behavior | CPU pixel copies |
|---|---|---|---|
| Decode, protocol 1.8 async/B-frame | QTI UBWC → Turnip/Vulkan → preallocated LINEAR; export returns immediately and `vaSyncSurface` completes content/fence | `linear` uses the same path. `qcom` disables async reservation and fails unless direct QCOM is validated | 0 |
| Decode, synchronous actual LINEAR | Direct LINEAR | `qcom` rejects it | 0 |
| Decode, synchronous actual QCOM | Direct only with broker + consumer gates; otherwise one GPU repack to LINEAR | `qcom` fails if either gate is absent | 0 |
| Encode, internally allocated/OBS writable NV12 | Direct LINEAR PRIME to the broker Vulkan producer, then one GPU render into the MediaCodec input Surface | Remains LINEAR in every selector mode | 0 |
| Encode, external LINEAR DRM PRIME | Direct LINEAR to the broker Vulkan producer | `qcom` rejects an external LINEAR import | 0 |
| Encode, external QCOM NV12 DRM PRIME | Direct QCOM import only with descriptor + broker + validation gates; otherwise one QCOM→LINEAR GPU repack before the existing broker render | `linear` rejects QCOM. `qcom` fails instead of repacking when direct import is unavailable | 0 |

The encode output is an H.264 or HEVC byte stream. A compressed video
bitstream has no DRM modifier; modifiers describe only image memory exchanged
before encoding or after decoding.

## Gateway routing

The repack gateway defaults to `ADVC_REPACK_GATEWAY_OUTPUT=auto`. It advertises
protocol 1.8 async LINEAR reservation and therefore chooses LINEAR for the
B-frame-safe path. A non-reserved QCOM frame can pass through only after
`ADVC_REPACK_GATEWAY_QCOM_PASSTHROUGH=validated-downstream-import-v1`.
`ADVC_REPACK_GATEWAY_OUTPUT=qcom` also requires that token, disables async
LINEAR reservation, and returns unsupported for a LINEAR source. This keeps the
route visible in logs and prevents unsafe QCOM preallocation.

## Applications

Recommended session baseline:

```sh
export ADVC_VAAPI_DECODE_OUTPUT=auto
export ADVC_VAAPI_ENCODE_INPUT=auto
export ADVC_VAAPI_ASYNC_EXPORT=candidate-firefox-bframe-v1
```

The async token is a versioned validation gate, not a format selector. It is
required for protocol 1.8 preallocated LINEAR decode surfaces; without it the
driver deliberately stays on the older synchronous export path.

- OBS should create an ordinary NV12 VA encode surface and request writable
  DRM PRIME export. LinDeX returns modifier-0 LINEAR, snapshots the producer
  dma-buf fence at `EndPicture`, and reports zero CPU pixel copies.
- FFmpeg and GStreamer should use their standard VA-API hardware device,
  VA-surface, and DRM PRIME descriptor APIs. Set the direction-specific
  selector only for an isolated test; `auto` is the recommended deployment
  value.
- A downstream decoder importer must be validated for the exact QCOM modifier
  before direct QCOM is enabled. Otherwise keep LINEAR. System-memory download
  of decoded surfaces still needs `vaDeriveImage`/`vaGetImage`, which this
  driver does not currently implement.

For FFmpeg, `-hwaccel vaapi -hwaccel_output_format vaapi` keeps decoded frames
as VA surfaces; adding `hwdownload` selects the currently unsupported CPU
download API. A conventional encode chain using `format=nv12,hwupload` is
supported but is a CPU-origin upload and is not the OBS/DRM-PRIME zero-copy
case. For GStreamer, keep `memory:VASurface`/VA memory through the pipeline;
negotiating ordinary system-memory `video/x-raw` permits an upload or download
and changes the copy accounting. Plugin availability and element names differ
between the old `vaapi*` and new `va*` plugin families, so validate each exact
pipeline rather than treating registry discovery as an application pass.

Current evidence covers policy unit tests, host VA/video suites, ARM64 builds,
protocol 1.8 H.264 B-frame/no-B and HEVC decode smokes, and H.264/HEVC hardware
encode smokes. Direct external-QCOM encode import is implemented behind the
new gates but has not yet completed a reference-device application run, so it
must not be enabled in a release profile yet.
