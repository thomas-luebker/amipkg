## amipkg 0.7.6 — update one package, multi-platform packages, tidier system installs

### Update a single package

Both GUIs had **Update All** but no way to update just one thing — even though
the CLI has taken `amipkg upgrade <id>` all along. There is now an **Update**
button beside Update All (and *Package → Update*, shortcut G).

It is only active when there is genuinely something to do, and the same check
marks the list: outdated packages show **UPDATE** in the status column instead
of a flat "installed", so you can see what is worth acting on without running
Check Updates first.

### Packages can name several platforms

0.7.5 introduced the Architecture field. Real Aminet readmes list *several*
platforms, comma-separated — `m68k-amigaos,ppc-amigaos,ppc-morphos` — and 0.7.5
only understood a single value, so a multi-platform package was hidden
everywhere. It now runs wherever **any** of its listed architectures does.

**If you are on 0.7.5, upgrading matters:** the catalog already carries such a
package (`imp3handler`), and 0.7.5 cannot see it.

### Installing into `C:`, `LIBS:`, `DEVS:` … does the right thing

Point a package at a system drawer and, until now, the whole archive tree went
there — documentation and all. Worse, an archive bringing its own top drawer
landed *nested*, so a command installed to `C:` ended up at `C:Tool/Tool`, where
the shell will never find it.

System drawers hold flat, specific things, so:

- **programs go in flat** — `C:Tool`, not `C:Tool/Tool`
- **everything else goes beside them**, in `<install drawer>/<id>`, structure
  intact

Both halves are recorded, so `remove` still cleans up either way, and amipkg
tells you how the split went. **Nothing changes for ordinary app drawers** —
`SYS:Programs` behaves exactly as before.

### Also fixed

Three internal buffers were still sized for the short `AMIPKG:` prefix although
the resolved path has been absolute since 0.4.7 — on a machine with a long
install path that could have silently truncated a receipt or config path.

### Upgrading

```
amipkg upgrade
```
