# amipkg 0.4.8 — release notes

The "it's a real project now" release: amipkg has its own home — on your
Amiga *and* on GitHub.

## Headlines

- **amipkg lives in a drawer now — yours.** Extract the archive anywhere;
  binaries, signed catalog, cache and the receipt database stay together in
  that drawer (like `MUI:`), published as the single **`AMIPKG:`** assign.
  Two setups: **`Install`** (self-contained — *nothing* outside the drawer
  is ever touched; uninstall = delete the drawer) or **`Install-System`**
  (adds one clearly marked assign+path block to `S:User-Startup` so
  `amipkg` works in every Shell after boot). Even loose binaries
  self-bootstrap silently — no assign requesters, ever. The legacy
  `AMIGAIMAGER:` name is gone (old images are still read for migration).
- **New: the MUI front-end (`amipkg-mui`).** MUI 3.8+, resizable, real list
  columns, description pane — full feature parity with the GadTools GUI,
  maintained in lockstep. This release also closes the crash saga behind
  it: the cross-compiler was silently dropping variadic arguments in the
  MUI stubs, so every object got a garbage tag list — fixed at the root
  and validated on real hardware (A4000/68060).
- **New: `amipkg adopt <id> <drawer>`.** Already have an app installed
  somewhere? Point amipkg at its drawer: it inventories the files and takes
  over updates and removal, in place. (Danke, jdb78.)
- **New: GlowIcons-style icon.** A shaded 16-colour parcel (true OS 3.5
  colour-icon format, RLE like the originals) with the classic planar icon
  embedded as fallback — correct on every OS from 3.1 up.
- **Public source.** amipkg now lives at
  <https://github.com/thomas-luebker/amipkg> (Apache-2.0), with an
  on-Amiga install guide (INSTALL.md) and CI. The catalog stays at
  <https://github.com/thomas-luebker/amiga-pkg> — packaging your software
  is a five-minute pull request (PACKAGING.md).

## Also in 0.4

- **Docs on the Amiga:** `amipkg.guide` (AmigaGuide) ships in the drawer
  and in both GUIs' Documentation menu.
- **GadTools GUI polish:** fixed-width list font on proportional-font
  Workbenches, NewLook menus (correct colours), double-click for Info,
  window position remembered.
- **recipeSchema 2:** catalog recipes can target assign-absolute paths
  (self-update lands exactly on the running binaries); older clients
  refuse newer schemas cleanly instead of guessing.
- **Aminet mirrors** in the catalog (us./se. fallbacks), install-time
  download resume, live progress in both GUIs.
- Clearer messages everywhere: "No catalog yet — run amipkg update",
  reboot hint after installs that extend User-Startup, `amipkg doctor`
  environment checks.
- Catalog grown to 115+ packages (EaglePlayer, Visage, TwinVNC, Zaphod,
  FullPalette, NewMode, Madhouse, Avalanche, and more).

## Compatibility

- AmigaOS 3.x, any CPU from 68000 up; MUI GUI needs MUI 3.8+
  (`amipkg install mui38`).
- Existing installs upgrade via `amipkg upgrade amipkg`. 0.3 receipts and
  catalogs are read as-is; pre-0.4 `AMIGAIMAGER:` images keep working.
