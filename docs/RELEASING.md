# Release packaging and publication

[English](RELEASING.md) | [한국어](ko/RELEASING.md)

This is the release-owner checklist. A locally built ZIP is not a release until
the exact source revision, package verifier, device matrix, documentation, and
published checksums all agree.

## Public source boundary

The public repository is expected to contain:

- root project, attribution, security, contribution, and bilingual user docs;
- `.github/workflows/verify.yml`, `.gitattributes`, and `.gitignore`;
- reviewed source under `src/`;
- the shipping module tree under `module/`;
- release, verification, test, and historical provider builders under
  `scripts/`; and
- exact prebuilt runtime files only where their checksum manifests authorize
  them.

It must exclude rootfs, unreviewed runtime/provider archives, build/dist/work
trees, local binaries, device or ADB logs, credentials, cookies/tokens, device
serials, private or paid appearance assets, and historical compositor
experiments. The checksum-locked public Archcraft Sway asset archives named by
the source locks are reviewed release inputs, not untracked downloads. The
paired pages in [the documentation index](README.md) are the public v3
documentation surface; ignored historical notes are not part of the release.

## Reviewed build inputs

- a reviewed LinDeX source revision;
- the canonical rootfs carrier derived from av2xn/Magisk-Debian-Chroot;
- pinned KGSL Mesa URLs and SHA-256 values;
- Android codec binaries matching `module/advc-artifacts.sha256`;
- bridge payloads matching their checksum manifests; and
- the official public Archcraft Sway source archive and the LinDeX-built
  appearance aggregate matching `module/profile-assets/SOURCES.lock` and
  `module/profile-assets/APPEARANCE_SOURCES.lock`.

The second appearance file is the LinDeX-built aggregate
`lindex-archcraft-sway-public-assets-v2.tar.gz`, SHA-256
`4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c`.
It is not an upstream Archcraft distribution archive. Rebuilding or replacing
it requires reviewing every public source path and commit in the appearance
lock, including the physically included Qogirr corresponding SVG source and
`COPYING` file.

Never add a private/paid Ko-fi archive, authentication material, full device
log, device serial, or local absolute path to source or a release attachment.
Archcraft's Ko-fi page is optional support for its creator, not an install
source, required payment, or license condition.

## Package and verify

Build release and dev ZIPs separately:

```powershell
./scripts/package-v3-module.ps1 -BaseModuleZip C:\path\to\base.zip -Flavor release
./scripts/package-v3-module.ps1 -BaseModuleZip C:\path\to\base.zip -Flavor dev

pwsh ./scripts/verify-v3-module.ps1 -ModuleZip ./dist/LinDeX-v3.0.4.zip -ExpectedFlavor release
pwsh ./scripts/verify-v3-module.ps1 -ModuleZip ./dist/LinDeX-v3.0.4-dev.zip -ExpectedFlavor dev
```

The verifier requires PowerShell 7 or newer. Windows PowerShell 5.1 is not a
supported release-validation environment.

The verifier must confirm module identity, rootfs hash and size, required
bridge/codec/profile files, the three profile contracts, official Sway asset
source locks and checksums, absence of compositor patch payloads, absence of
`system/`, exclusion of development-only diagnostic executables, and the
selected logging flavor.

Release mode must create no persistent setup/session/codec logs and must remove
stale equivalents. Dev mode may keep only bounded rotated logs. Generate
artifact checksums after verification; do not publish a historical or
repository-level checksum file as if it described the new ZIPs.

## Device acceptance

1. Confirm DP is physically detached.
2. Install the exact release ZIP through KernelSU.
3. Reboot Android normally to activate the root module.
4. Record the Git commit and release ZIP SHA-256.
5. Verify rootfs creation/update, WebUI state, Mesa, the no-chroot-PolicyKit
   contract, all three profiles, and the Sway Dark/Light/Pywal selection.
6. For every profile, run start, stop, forced unplug, and reconnect.
7. Verify physical connector/EDID gating and session/process-group cleanup.
8. Verify release no-log policy and repeat with the dev ZIP for bounded logs.
9. Complete the codec rows, including explicit `modifier = 0` LINEAR validation
   and fail-closed decode PRIME behavior.
10. Complete the USB dock matrix and direct-scanout evidence row.

Write results into [Validation status](VALIDATION_STATUS.md). Isolated probes
remain labeled and do not replace packaged-module acceptance.

## Public preflight checklist

### Repository and documentation

- [x] English `README.md` and Korean `README.ko.md` are separated and linked.
- [x] Public end-user and release docs have paired EN/KO pages and an index.
- [x] Project ancestry, MIT notices, third-party ownership, and Archcraft/Ko-fi
  boundaries are explicit.
- [x] Public docs describe an unmodified-compositor bridge and do not present
  historical compositor experiments as a release path.
- [x] Public docs exclude development-only diagnostic executable deployment.
- [x] `.gitignore` excludes logs, secrets, build trees, downloaded archives,
  historical experiments, and non-public work notes.
- [ ] Re-run local-link, ignored-file, and secret scans on the exact tag.
- [ ] Confirm GitHub repository URL, issue/security contact route, and release
  URL once the repository exists.

### Build and hardware

- [ ] Run CI and every package verifier on the exact tagged commit.
- [x] Build both flavors reproducibly and record SHA-256 values.
- [ ] Install the exact release ZIP with DP detached and reboot Android normally.
- [ ] Complete the three-profile final device matrix.
- [x] Complete bounded packaged-module application encode measurements on the
  reference device.
- [ ] Complete modifier `0` LINEAR decode/import validation.
- [ ] Confirm H.264, HEVC Main, and VP9 Profile 0 remain unadvertised unless each profile's exact immutable/live preflight passes.
- [ ] Complete release/dev logging, USB dock, hotplug, and direct-scanout gates.

### Publication

- [x] Update `VERSION`, changelog date, validation date, artifact names, and
  hashes together.
- [ ] Attach the rootfs-inclusive release ZIP, dev ZIP only if intentionally
  published, checksums, source archive, and required third-party source offer.
- [ ] Re-download attachments and verify hashes before announcing the release.
- [ ] Keep all still-pending rows labeled; do not replace evidence with a
  blanket “production ready” statement.
