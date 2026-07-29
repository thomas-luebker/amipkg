## amipkg 0.7.3 — installing into system drawers

### Installing into `C:` (or `LIBS:`, `DEVS:`, `S:` …) now does the right thing

Point a package at a system drawer and, until now, the whole archive tree went
there — documentation and all. Worse, an archive that brings its own top drawer
landed *nested*, so a command installed to `C:` ended up at `C:Tool/Tool`, where
the shell will never find it.

System drawers hold flat, specific things, so that is what amipkg does now:

- **programs go in flat** — `C:Tool`, not `C:Tool/Tool`
- **everything else goes beside them**, in `<install drawer>/<id>`, with its
  structure intact

What counts as belonging there is deliberately careful: name conventions first
(`.library`, `.device`, `.class`, `.font`, `.prefs`) and the executable header
for the drawers that hold plain commands. Anything unrecognised goes to the
companion drawer, where it does no harm. Both halves are recorded, so `remove`
still cleans up either way, and amipkg tells you how the split went.

**Nothing changes for ordinary app drawers.** Installing into `SYS:Programs` —
what almost everything does — behaves exactly as before. This only engages when
you deliberately choose a system drawer, which pairs with the per-package
install drawer added in 0.7.2:

```
amipkg install <id> DIR=C:
```

### Also fixed

Three internal buffers were still sized for the short `AMIPKG:` prefix, although
the resolved path has been absolute since 0.4.7. On a machine with a long
install path that could have silently truncated a receipt or config path.

### Upgrading

```
amipkg upgrade
```

Thanks to **yelworC** and **djbase**, whose reports drove both 0.7.2 and this.
