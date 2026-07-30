## amipkg 0.7.7 — updating amipkg can no longer leave you without amipkg

A user updating from the GUI on an A4000 watched the program disappear from
its drawer mid-update. Nothing was lost — the new files were on disk, in the
home drawer — but the app he had launched was gone, and that should not be
possible. This release makes it impossible.

### An upgrade no longer removes before it installs

Upgrading any package used to remove the old version first and install the new
one after. For any other program that is merely tidy. For **amipkg upgrading
itself** it means a moment when there is no amipkg on the machine at all and no
second copy anywhere — and anything that goes wrong in the copy phase leaves
you with nothing to run.

amipkg now installs **over** the previous version and cleans up afterwards,
once the replacement is known to be on disk. If a self-upgrade fails now, the
version you were running is still there, still working.

The cleanup is deliberately narrow: it only removes files inside amipkg's own
home drawer, and only ones the new version genuinely no longer ships. A stale
file left somewhere else is harmless; deleting something outside the home on
the strength of an old record is exactly how a running program gets erased.

### `PROGDIR:` no longer goes into the receipt

The record of amipkg's own files listed them as `PROGDIR:amipkg` and so on.
`PROGDIR:` means "the drawer of the program running right now" — fine when it
is written, wrong when it is read back later by a *different* amipkg, such as
the CLI a GUI starts. Since an upgrade deletes the recorded paths, a record
written in one drawer could delete the app in another.

Paths in the receipt are now absolute, and match the names the installer
writes, so the two always describe the same files.

**Receipts already written this way are repaired automatically** the next time
amipkg starts. Every machine that has run an earlier version has one, so
fixing only new installs would have left them as they were.

### If an update ever looks like it emptied the drawer

It probably did not. Workbench does not redraw an open window when files are
replaced underneath it — close and reopen the drawer, or check from a Shell
with `List`. And a complete copy of the new version is always kept at
`<amipkg drawer>/cache/extract` until the next download.

### Upgrading

```
amipkg upgrade
```

Nothing else changed: same catalog, same repositories, same packages.
