# Security policy

[English](SECURITY.md) | [한국어](SECURITY.ko.md)

## Reporting

Please report vulnerabilities privately to the repository maintainer before
opening a public issue containing exploitation details. Do not include device
serials, account tokens, Ko-fi cookies, or complete Android logs.

## Security boundaries

LinDeX handles privileged DRM descriptors, Linux input devices, a root-owned
Android service, and a Debian chroot. Reports involving descriptor leakage,
lease lifetime, archive traversal, unsafe extraction, command injection,
WebUI command execution, or unintended Android input capture are security
relevant.

The project does not support changes that remount or modify Android `system`,
`vendor`, or `product`. Paid third-party assets and authentication material are
outside the trusted release boundary.
