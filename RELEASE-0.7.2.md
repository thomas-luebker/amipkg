## amipkg 0.7.2 — from the first week of external repos

Everything here comes from testers running 0.7.x for real. Thanks to **djbase**,
who set up the first external repository, and **yelworC** for the install-drawer
reports.

### Choose where each package goes

One global install drawer was not enough — a tool may belong in `C:` while
everything else belongs in `SYS:Programs`. You can now decide per package:

```
amipkg install <id> DIR=<drawer>    install there, and remember it
amipkg dir <id> <drawer>            set one package's drawer
amipkg dir <id> -                   clear it, follow the global again
```

The choice is stored *before* the install, so it governs later upgrades too.
Both GUIs get **Install To…** next to Install.

### Adopt now reads the version that is actually installed

`amipkg adopt` used to record "unknown" unless you passed a version by hand,
and the GUIs then quietly displayed the *catalog's* version instead — which
looked right until you disabled the repository and it vanished.

Adopt now reads the `$VER:` string out of the binaries, exactly as `C:Version`
does, so what you see is what is on your disk. When a version genuinely has to
come from the catalog it is shown as `1.3?`, because it is a guess and should
look like one.

*Already adopted something? Adopt it once more to pick up the real version.*

### Hosting your own repository: corrected docs, and a safety net

The catalog example in `HOSTING.md` was wrong in two ways, and both made the
client reject the catalog outright:

- `"schema"` must be exactly **1** — it is the format version, not yours
- the download belongs in `archive` (`url` / `sha256` / **`sizeBytes`**), not in
  `install`

Both are fixed. More importantly, `amipkg-repo-sign` now **validates before it
signs** and refuses to produce a signature for a catalog the Amiga could not
read, so a typo fails on a machine with a screen instead of on someone's A4000:

```
amipkg-repo-sign check packages.json     lint without signing
```

### Upgrading

```
amipkg upgrade
```
