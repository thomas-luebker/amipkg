## amipkg 0.7.5 — an Architecture field, so a catalog can serve more than 68k

Suggested by **djbase**, who runs the first external repository and had no way
to publish anything that isn't 68k.

### `requirements.architecture`

Aminet's vocabulary, so you write what you already know:

```json
"requirements": { "architecture": "ppc-morphos" }
```

`m68k-amigaos`, `ppc-morphos`, `ppc-amigaos` (OS4), `i386-aros` /
`x86_64-aros` / `ppc-aros` / `arm-aros`, `ppc-warpup`, `ppc-powerup`, and
`generic` for documentation or data that runs anywhere.

**Absent means `m68k-amigaos`.** The catalog predates the field, so the default
has to be the platform everything was written for — every existing entry stays
valid, and nothing changes on a classic Amiga.

### It filters, and it stays honest about it

amipkg now detects what it is running on and hides packages that machine cannot
use — refusing to install one if you ask directly, alongside the existing CPU
and Kickstart checks. Both GUIs do the same and show the architecture in Info.

Two things worth knowing:

- **MorphOS and AmigaOS 4 still see the whole 68k catalog.** Both run m68k
  binaries, so the filter removes only what is genuinely unusable rather than
  leaving those machines with almost nothing. (amipkg itself is an m68k binary,
  which is exactly why it can already run there.)
- **Nothing disappears without a way to look.** `avail` reports how many entries
  it hid, and `amipkg avail ALL` lists everything with the architecture named.

### For repo owners

`HOSTING.md` covers the field. An architecture outside the list is a validation
**error**, not a warning — an unrecognised value would silently hide the package
from every machine, which is worse than a typo you can see.

### Upgrading

```
amipkg upgrade
```
