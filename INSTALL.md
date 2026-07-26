# Installing amipkg on your Amiga

Ten minutes, start to finish. Applies to real hardware and emulators alike.

## What you need

- An Amiga with **AmigaOS 3.x** (2.04 is the technical minimum; 3.1+
  recommended). Any CPU from a plain 68000 up.
- **`C:lha`** to extract the archive. Almost every system has it; if not,
  get `lha.run` from Aminet (`util/arc/lha.run`) — it is a self-extracting
  binary that needs no extractor itself.
- About **1 MB free disk space** for amipkg, plus room for the software you
  install (default install drawer: `SYS:Programs`, changeable).
- For downloads: a **TCP/IP stack** (Roadshow, AmiTCP, Genesis — or the
  emulator's built-in `bsdsocket.library` in UAE/Amiberry/FS-UAE). Browsing
  the bundled catalog works completely **offline**.
- Optional: **AmiSSL 5.x** for packages hosted on https-only sites. amipkg
  itself and most catalog software come over plain HTTP (Aminet), which
  needs nothing.

> **Amiga Imager users:** images built by
> [Amiga Imager](https://amiga-imager.com) already ship amipkg preinstalled
> and integrated — skip to [First steps](#first-steps).

## Step 1 — Get amipkg.lha onto the Amiga

Download **[amipkg.lha](https://thomas-luebker.github.io/amiga-pkg/amipkg.lha)**
(≈580 KB) on any modern machine, then use whichever route fits your setup:

- **Emulator (UAE / Amiberry / FS-UAE):** put the file in a directory that is
  mounted as an Amiga drive (a "host directory" hard drive), or copy it into
  the mounted `.hdf` with your emulator's tools.
- **CF/SD card:** if your Amiga mounts a FAT-formatted card (CF-IDE adapter,
  fat95, CrossDOS), copy the file onto the card on the PC and read it on the
  Amiga.
- **Floppy:** it fits on a single DD disk. Write an ADF containing the file
  with your favorite tool (or use a Greaseweazle/KryoFlux), or copy it onto a
  PC-formatted 720 KB disk and read it with CrossDOS.
- **Network:** if the Amiga is already online, any FTP/SMB/HTTP client works
  — e.g. download it directly with an Amiga browser or `wget`-style tool
  from the URL above.

## Step 2 — Extract into a drawer of your choice

amipkg **lives in the drawer you put it in** (like `MUI:`) — binaries, the
signed catalog, cache and its database all stay together there. Pick any
place you like:

```
MakeDir Work:AmiPKG
LhA x amipkg.lha Work:AmiPKG/
```

(Or extract with your file manager of choice — DOpus users know what to do.)

## Step 3 — Run ONE of the two setups

Open the drawer in Workbench and double-click one icon (or `Execute` it from
a Shell CD'd into the drawer):

| | **Install** (self-contained) | **Install-System** (integrated) |
|---|---|---|
| Where amipkg lives | this drawer | this drawer |
| Startup-Sequence / User-Startup | **never touched** | one clearly marked block added to `S:User-Startup` (assign + Shell path) |
| `amipkg` in a Shell | CD into the drawer first | works from **every** Shell, right after boot |
| Undo | delete the drawer | delete the block, then the drawer |

Not sure? Take **Install** — you can run Install-System later at any time.
Both seed amipkg's own receipt, so `amipkg upgrade` keeps amipkg itself
up to date from day one.

## First steps

The bundled catalog is signed and works offline immediately:

```
amipkg avail              the whole catalog (100+ packages)
amipkg avail vnc          search it
amipkg info twinvnc       details for one package
```

Go online (start your TCP/IP stack), then:

```
amipkg update             fetch the latest signed catalog
amipkg install visage     download, verify, resolve deps, install
amipkg check              what has updates?
amipkg upgrade            update everything (including amipkg itself)
```

Prefer clicking? **`amipkg-gui`** (runs everywhere, zero dependencies) or
**`amipkg-mui`** (MUI 3.8+ — `amipkg install mui38` gets you MUI) offer the
same features: browse, search, install with a plan preview, update all,
adopt, remove. The full manual is right in the drawer:
**`amipkg.guide`** (also in both GUIs' Documentation menu).

## Already have some of the software?

You installed Visage years ago in `Work:Grafik/Visage`? Don't reinstall —
**adopt** it:

```
amipkg adopt visage Work:Grafik/Visage
```

amipkg inventories the files and takes over updates and removal, in place.

## Uninstalling

- Self-contained: delete the drawer. That's everything.
- Integrated: also remove the marked `; amipkg home` block from
  `S:User-Startup`.
- Software installed *by* amipkg: `amipkg remove <id>` first (receipt-driven
  — it deletes exactly what was installed, including any `User-Startup`
  lines a package added).

## Troubleshooting

The `amipkg.guide` in the drawer has a full troubleshooting section
(no stack running, https without AmiSSL, disk full, CPU/OS floors, moved
drawer). Quick health check any time: `amipkg doctor`.

Bugs and questions: <https://github.com/thomas-luebker/amipkg/issues>
