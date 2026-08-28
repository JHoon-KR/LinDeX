# Contributing to LinDeX

[English](CONTRIBUTING.md) | [한국어](CONTRIBUTING.ko.md)

LinDeX accepts focused changes to the unmodified-compositor bridge, module,
WebUI, input routing, video bridge, tests, and public documentation.

## Rules

- Preserve av2xn's MIT notice and all third-party notices.
- Do not add paid profile assets, authentication cookies, device serials, ADB
  logs, rootfs archives, or downloaded Mesa archives to Git.
- Do not write Android `/system`, `/vendor`, or `/product`.
- Do not add compositor-version patches to the v3 release path.
- Keep unsupported capabilities fail-closed. A feature bit requires a live
  probe of the exact backend it represents.
- Treat modifier value `0` as DRM LINEAR only when the descriptor explicitly
  declares it; never use `0` as a shortcut for missing validation.
- Strict zero-copy must never silently perform a CPU pixel copy.
- Scope cleanup to the recorded session PID/process group and file descriptors.

## Checks

Before opening a change:

```sh
node --check module/webroot/app.js
node --check module/webroot/locales.js
cmake -S src/video_bridge -B build/video-host
cmake --build build/video-host
ctest --test-dir build/video-host --output-on-failure
```

Run the current shell fixtures relevant to the changed component. Release ZIPs
must also pass `scripts/verify-v3-module.ps1`.

Hardware validation reports should identify the device family and Android
version without publishing a unique device serial. Attach only the smallest
sanitized log needed to reproduce a failure.

Public user or release documentation changes must update the paired English and
Korean page in the same change. English remains normative for exact identifiers
and command syntax.
