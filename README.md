# amipkg — a package manager for AmigaOS 3.x

**`amipkg install <id>`** on a real Amiga (or UAE), and the software is there:
downloaded, SHA-256-verified against a **cryptographically signed catalog**,
dependencies resolved for your CPU, and recorded in a receipt database so it
can be removed again cleanly. Think Homebrew or apt — for a 68000 from 1985
upwards.

- **Runs everywhere:** any AmigaOS 3.x system (2.04 minimum), real hardware
  or emulator, from a stock A500 to a Vampire. Plain-HTTP downloads work
  without TLS; https hosts work when AmiSSL 5.x is installed.
- **Three front-ends, one logic:** the `amipkg` CLI, a zero-dependency
  GadTools GUI, and a MUI GUI — the GUIs drive the CLI, so behavior never
  drifts.
- **Trust without trusting the network:** the catalog is signed offline with
  an Ed25519 key and verified **on the Amiga**; every archive must match the
  SHA-256 pinned inside that signed catalog. The download host is never
  trusted.
- **Lives in one drawer** (like `MUI:`): binaries, catalog, cache and receipt
  DB together, published as the `AMIPKG:` assign. Fully self-contained by
  default — installing it never touches your Startup-Sequence unless you
  explicitly choose the integrated setup.

## Install it on your Amiga

Download **[amipkg.lha](https://thomas-luebker.github.io/amiga-pkg/amipkg.lha)**
(≈580 KB — fits on one DD floppy) and see **[INSTALL.md](INSTALL.md)** for the
step-by-step guide: getting the archive onto the Amiga, the two setup
flavours, and your first `amipkg install`.

## The catalog

Packages live in the companion repo
**[amiga-pkg](https://github.com/thomas-luebker/amiga-pkg)** — one JSON file
per package, validated by CI, published as a signed index. Want your software
installable on every amipkg system? A pull request takes about five minutes:
[PACKAGING.md](https://github.com/thomas-luebker/amiga-pkg/blob/main/PACKAGING.md).

## Commands

```
amipkg update                 fetch + verify the latest signed catalog
amipkg avail [term]           browse / search the catalog
amipkg info <id>              package details
amipkg install <id> [DRYRUN]  dependency-resolved, space-checked install
amipkg upgrade [<id>]         update everything out of date (incl. itself)
amipkg check                  which installed packages have updates
amipkg list                   what is installed
amipkg adopt <id> <drawer>    take over managing an app you already have
amipkg remove <id>            receipt-driven clean uninstall
amipkg doctor                 audit installs against the receipts
amipkg dir [<path>]           show/set the install drawer
amipkg repo                   list / add / remove repositories
```

Install a specific repository's build with `repo:package`, e.g.
`amipkg install mystuff:ibrowse`.

## Building from source

Cross-compiled with [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc)
(m68k-amigaos-gcc 6.5):

```
make all            # amipkg + amipkg-gui + amipkg-mui (68000 baseline)
make -f Makefile.host test   # portable-core test suite on macOS/Linux
```

`dist/make-bundle.sh` builds the release archives (deterministic lh0 — the
same source always produces the same SHA-256).

- `src/core/` — portable C99: JSON/index parsing, version compare, dependency
  resolver, recipe → execution plan, receipts, SHA-256, Ed25519
  ([TweetNaCl](https://tweetnacl.cr.yp.to/)). Fully host-testable.
- `src/amiga/` — the AmigaOS layer: dos.library install engine, bsdsocket
  HTTP (+ optional AmiSSL https), GadTools GUI, MUI GUI, ADF extraction.
- `vendor/` — build-time headers only (MUI 3.8 developer kit, AmiSSL SDK);
  see the READMEs there for provenance and licenses.

## Host your own repository

amipkg installs from **more than one repository**, and anyone can run one. A
repository is just two static files on any web server — `packages.json` and its
signature. No database, no PHP, no HTTPS required.

```
amipkg repo add mystuff http://your.host/amiga <public-key>
amipkg update
```

Repositories are consulted in order and the first one that has a package wins,
so `amipkg repo up`/`down` decides what you get; `amipkg info` shows which
repository anything came from. Dependencies resolve across repositories.

Signing your repo takes about a minute and gives your users exactly the same
guarantee as the official catalog — `tools/amipkg-repo-sign` makes a keypair
and signs, with no dependencies beyond Python 3. Unsigned repositories are
allowed too; amipkg explains the trade-off and asks once.

**→ [HOSTING.md](HOSTING.md)** walks through both paths.

## Trust model

See [SECURITY.md](https://github.com/thomas-luebker/amiga-pkg/blob/main/SECURITY.md)
in the catalog repo. Short version: the signing key lives offline; the
catalog carries the signature; the Amiga verifies it locally; archives are
pinned by SHA-256. Security contact: **thomas@amiga-imager.com**.

Each repository is verified against **its own** pinned public key, so one
repository's signature can never vouch for another's catalog. An unsigned
repository is opt-in per repo and stays labelled as such everywhere it appears
— see [HOSTING.md](HOSTING.md) for what that costs and why.

## Part of the Amiga Imager project

amipkg ships preinstalled on every image built by
[Amiga Imager](https://amiga-imager.com) — but it is an independent tool and
works on any Amiga, however it was set up.
