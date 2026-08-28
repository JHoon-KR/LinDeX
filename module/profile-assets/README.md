# Official Archcraft Sway Free assets

LinDeX bundles pinned, checksum-verified copies of Archcraft's public
GPL-3.0 Sway Free configuration and its default public GTK, icon, and cursor
themes. This keeps one-button installation reproducible and independent of
network availability.

- `archcraft-sway-free-e4d0126d.tar.gz`: complete Git archive of
  `archcraft-os/archcraft-sway` commit
  `e4d0126d7f236fee50a84fbb0e61498dcf5705e7`.
- `lindex-archcraft-sway-public-assets-v2.tar.gz`: a LinDeX-generated,
  reproducible aggregate of the public Dark and Light GTK, icon, and cursor
  assets required by the official profile. It is not an upstream Archcraft
  archive; every included path is pinned in `APPEARANCE_SOURCES.lock`. The
  Qogirr-Dark cursor's corresponding SVG source and GPL copying terms are
  included under `corresponding-source/Qogirr-Dark` inside the archive.
- `SOURCES.lock` and `APPEARANCE_SOURCES.lock`: exact source URLs, commits,
  paths, install destinations, and licenses.
- Adjacent `.sha256` files: release verification digests.

The upstream GPL license texts are preserved inside the archives. LinDeX's
Debian adapter is distributed as source in
`module/payload/debian/android-drm-profile-manager` and records its changes in
the installed profile's `.lindex-source` file.

If you enjoy the visual design, you can optionally thank and support its creator
through the [Archcraft Sway Ko-fi page](https://ko-fi.com/s/10f2e87af3).
This support link is not an installation source, purchase requirement, or
license condition for the public GPL-3.0 profile bundled by LinDeX.
