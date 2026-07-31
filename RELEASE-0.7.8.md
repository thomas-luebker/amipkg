## amipkg 0.7.8 — the Refresh button comes back, and one word for one action

### Refresh existed in the menu but not in the window

The GadTools GUI's button column grew an entry in 0.7.6 (**Update Selected**)
and the loop that creates those buttons was never told there was one more — so
the **last** button in the list, **Refresh**, simply stopped being drawn. Two
releases with a button that the code still handled, still enabled and disabled
correctly, and never showed.

It is back, and the loop now counts the buttons itself so adding one cannot do
this again.

### One word for one action

The same action was called three different things depending on where you looked:
the GadTools GUI said **Update**, the MUI GUI said **Upgrade Selected**, and
both then asked *"Upgrade every out-of-date package?"* after you pressed
**Update All**. The CLI printed *"Upgrade amipkg 0.7.7 -> 0.7.8"* in the log
window underneath.

Everything now says **update**. Both GUIs show **Update Selected** next to
**Update All** — with the scope spelled out, because a bare "Update" beside
"Update All" reads like a weaker version of the same command rather than
"just this one".

The command you type is unchanged: `amipkg update` still refreshes the catalog
and `amipkg upgrade` still installs newer versions. Only what you read changed,
so existing scripts and habits keep working.

### "no output" now says what it means

When a shelled-out command produced nothing at all, the GUIs reported
*"(no output — check RAM:amipkg-gui.out)"* and sent you to an empty file.

An empty output file does not mean the command failed quietly — it means the
command **never started**, and on a 68k machine that is nearly always memory:
amipkg has to load and hold the whole catalog (200+ packages) on top of a
resident Workbench, MUI and dock. The requester now says that and tells you
what to do about it, including the setting emulator users need.

### Upgrading

```
amipkg upgrade
```
