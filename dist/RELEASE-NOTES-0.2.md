# amipkg 0.2 — the AmigaOS 3.x package manager

The first public release of amipkg: install, update, and remove software on
any AmigaOS 3.x system from a curated, cryptographically signed catalog of
100+ packages. No other tool like it exists for classic AmigaOS.

## Assets

- **`amipkg.lha`** — the full tester bundle: CLI + GUI + the signed catalog
  seeded for offline browsing + a double-clickable Install script. START HERE.
- **`amipkg-client.lha`** — binaries only; this is what `amipkg upgrade
  amipkg` fetches for self-updates. You normally don't download it by hand.

## What it does

- **Trust**: the catalog is Ed25519-signed offline and verified on-device;
  every archive download is checked against a SHA-256 pinned in that signed
  catalog. A tampered catalog or archive is refused.
- **Dependencies**: `install` resolves and installs dependencies first,
  picking the right CPU variant for your machine (68000…68060).
- **HTTPS**: with AmiSSL 5.x installed, https-only hosts (GitHub releases)
  work too; plain-HTTP Aminet needs nothing.
- **GUI** (GadTools, runs on any 3.x): browse/search, category filter,
  newest-first sort, background installs with live progress, Run an installed
  program, one-click catalog + package updates.
- **Self-update**: `amipkg upgrade amipkg`.
- **Safety**: receipt database for clean removal, disk-space preflight,
  build-time-only packages refused with a clear message.

## Requirements

AmigaOS 3.0+, `C:lha`, a bsdsocket TCP/IP stack for downloads (Roadshow /
AmiTCP / Miami / an emulator's bsdsocket). AmiSSL 5.x optional (https hosts).

## Install

Extract `amipkg.lha`, then double-click the **Install** icon (or `Execute
Install` from a Shell). See the bundled ReadMe for the full manual.

Catalog + package submissions: https://github.com/thomas-luebker/amiga-pkg
