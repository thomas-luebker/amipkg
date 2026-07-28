## amipkg 0.7.1 — interface polish

A follow-up to 0.7.0 that brings both GUIs in line with the
[MorphOS/MUI Style Guide](https://library.morph.zone/Style_Guide). No functional
changes — everything from 0.7.0 (multiple repositories, the repository manager,
`repo:package`) works exactly as before.

### What changed

**A proper Settings menu.** *Install Drawer…* and *Repositories…* moved out of
*Package* — they configure amipkg, they aren't things you do to a package. The
MUI build also gains **Settings → MUI…** for the local interface settings, as
every MUI application should.

**The first menu is now named after the app** rather than *Project*, which by
convention belongs to programs that load and save project files.

**`?` opens About**, the standard shortcut everywhere on Amiga. It had been
sitting on *Documentation*.

**Input fields are labelled with a colon**, so it reads as `Name:` / `URL:` /
`Public key:` and it's clear which field a label belongs to.

All of it applies to the **GadTools build as well as the MUI one** — these are
Amiga interface conventions, not MUI-only ones, and the two front-ends are kept
deliberately identical.

### Not adopted

The guide suggests `Aboutbox.class` for a uniform About window. amipkg sticks to
**MUI 3.8 stock classes** so it runs on a plain OS 3.x install with no extra
class installed — that matters more here than a tidier About box.

### Upgrading

```
amipkg upgrade
```
