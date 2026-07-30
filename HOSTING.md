# Hosting your own amipkg repository

amipkg can install from more than one repository. Anyone can host one: a club,
a developer shipping their own builds, or just you, for your own machines.

A repository is **two static files on any web server**:

```
http://your.host/amiga/packages.json       the catalog
http://your.host/amiga/packages.json.sig   its signature (if you sign it)
```

No database, no PHP, no HTTPS required. GitHub Pages, a Raspberry Pi, the
webspace your ISP gave you in 2003 — all fine.

Users add it on the Amiga with:

```
amipkg repo add mystuff http://your.host/amiga <your-public-key>
amipkg update
```

---

## Sign it (recommended)

**Why it matters:** amipkg has no TLS, so catalogs travel over plain HTTP. That
is safe *when the catalog is signed*, because amipkg verifies the signature on
the Amiga before it will use it, and the archive checksums it then trusts live
inside that verified catalog. Nobody between you and your users can change what
they install.

If you *don't* sign, that chain is broken end to end. An unsigned catalog can be
rewritten in transit, and the checksums do **not** save your users, because
those checksums are inside the very catalog that was rewritten. So your users
are not only trusting you — they are trusting their ISP, their router, and any
proxy in between. amipkg will warn them and make them opt in by hand.

Signing takes about a minute and costs nothing.

### 1. Make a keypair (once, ever)

```
$ tools/amipkg-repo-sign keygen mystuff

Secret key : mystuff.secret   (KEEP THIS PRIVATE - never put it on the web server)
Public key : mystuff.public

Your public key is:

    o4fnZrdL0RIBQG+IVef6ild5ad2fn1yAdF6EgIl+OAc=
```

The tool needs **nothing installed** — Python 3 and nothing else. (It does not
use OpenSSL on purpose: macOS ships LibreSSL, which cannot do Ed25519 at all.)

**The secret key is the whole thing.** Anyone holding it can publish packages
that your users' machines accept without question.

- Keep it **off the web server** that serves the repo.
- Back it up somewhere you will not lose it — if it is gone, you cannot publish
  updates your existing users will accept, and everyone has to re-add the repo
  with a new key.
- Do not commit it to a public git repo. If you keep the repo in git, add
  `*.secret` to `.gitignore` before the first commit.

### 2. Sign the catalog

Every time the catalog changes:

```
$ tools/amipkg-repo-sign sign mystuff.secret packages.json
Signed packages.json (107714 bytes)
Wrote  packages.json.sig
```

Upload **both** files. A stale signature is worse than none: the catalog will
simply fail to verify and your users keep whatever they had.

### 3. Publish your public key

Put the public key string wherever people find your repo — your README, forum
post, or web page. It is not a secret; it is the thing that makes you
impossible to impersonate.

```
amipkg repo add mystuff http://your.host/amiga o4fnZrdL0RIBQG+IVef6ild5ad2fn1yAdF6EgIl+OAc=
```

### Checking before you publish

```
$ tools/amipkg-repo-sign verify o4fnZrdL0RIBQG+IVef6ild5ad2fn1yAdF6EgIl+OAc= packages.json
OK: packages.json verifies against that public key.
```

---

## Without signing

If you really want to skip it, users can add the repo with no key:

```
amipkg repo add mystuff http://your.host/amiga
```

amipkg explains the risk and asks for confirmation once. The repo is then
permanently labelled `UNSIGNED` in `amipkg repo` and in both GUIs. Nothing else
changes — packages install normally.

This is reasonable for a repo on your own LAN. It is a poor choice for anything
you hand to strangers over the internet.

---

## Priority, and how to not surprise people

Repositories are consulted **in order**, and the first one that has a package
wins. Order is the user's choice, and amipkg does not treat signed repos as
more important than unsigned ones — the list is exactly what the user asked for.

That has a consequence worth knowing as a repo owner:

> If your repo sits above the official one and you publish a package id that
> already exists there — `ibrowse`, say — your build is what your users get,
> including on `amipkg upgrade`.

That is a legitimate thing to do (a patched build, a newer version). It is also
a good way to surprise people by accident. So:

- **Use your own ids** unless you specifically intend to replace a package.
- If you *do* intend to replace one, say so plainly where you publish the repo.

Users can always reach a specific repo's build by name, whatever the order:

```
amipkg install mystuff:ibrowse      your build
amipkg install official:ibrowse     the official one
amipkg info mystuff:ibrowse         which is which
```

And they can see where anything came from with `amipkg info <id>` (the `repo:`
line) or in the GUI package list.

---

## The catalog format

`packages.json` is the same format the official repo uses:

```json
{
  "schema": 1,
  "indexVersion": 1,
  "generated": "2026-07-29",
  "packages": [
    {
      "id": "mytool",
      "name": "My Tool",
      "version": "1.0",
      "category": "Utilities",
      "description": "Does a thing",
      "archive": {
        "url": "http://your.host/files/MyTool.lha",
        "sha256": "…64 lowercase hex chars…",
        "sizeBytes": 12345
      },
      "install": {
        "sourceType": "url",
        "userVisible": true
      }
    }
  ]
}
```

Points that matter:

- **`"schema"` must be exactly `1`.** It is the format version, not your
  catalog's version — the client refuses to read anything else, and it refuses
  the *whole* catalog, not just one entry. Use `indexVersion` for your own
  numbering.
- **The download lives in `archive`, not in `install`** — `url`, `sha256` and
  `sizeBytes` (note: `sizeBytes`, not `size`). `install` only carries
  `sourceType` and `userVisible`.
- **`sha256` is required.** amipkg refuses to install an archive with no pin.
  It is what makes downloading the archive itself safe over plain HTTP. Lowercase
  hex.
- Re-generate `sha256` and `sizeBytes` whenever you replace an archive, and
  re-sign the catalog.
- `id` must be unique **within your repo**. See the priority note above for what
  happens when it collides with another repo's id.
- Archives can live anywhere — a different host, Aminet, GitHub releases. Only
  the catalog has to be at the repo URL. Optional `archive.mirrors` takes a list
  of fallback URLs.

### Packages for MorphOS, AmigaOS 4 or AROS

By default every entry is assumed to be `m68k-amigaos` — the whole catalog
predates the field, so absent has to mean the platform everything was written
for. To publish for another platform, say so:

```json
"requirements": { "architecture": "ppc-morphos" }
```

Aminet's vocabulary: `m68k-amigaos`, `ppc-morphos`, `ppc-amigaos` (OS4),
`i386-aros` / `x86_64-aros` / `ppc-aros` / `arm-aros`, `ppc-warpup`,
`ppc-powerup`, and `generic` for documentation or data that runs anywhere.

amipkg detects what it is running on and hides packages that machine cannot
use, refusing to install one if asked directly. Two things worth knowing:

- **MorphOS and AmigaOS 4 still see the whole 68k catalog**, because both run
  m68k binaries. The filter removes only what is genuinely unusable.
- `amipkg avail ALL` shows everything regardless, with the architecture
  named — so nothing is hidden without a way to look.

A value outside the list above is a validation **error**, not a warning: an
unrecognised architecture would silently hide the package from every machine.

### Check it before you publish

Do not find out on the Amiga. `amipkg-repo-sign` validates the catalog as it
signs, and refuses rather than producing a signature for something the client
cannot read:

```
$ tools/amipkg-repo-sign sign mystuff.secret packages.json
amipkg-repo-sign: packages.json: "schema" must be 1 (found 2)
```

The full schema, including dependencies, recipes and CPU/Kickstart floors, is in
[`schema/`](https://github.com/thomas-luebker/amiga-pkg) in the gateway repo,
and `PACKAGING.md` there walks through writing an entry.

---

## Submitting to the official repo instead

If you want your package available to everyone by default, you do not need your
own repo at all — send it to the official one. That catalog is human-reviewed
and signed offline, and packages in it reach every amipkg user with no setup.
See `CONTRIBUTING.md` in the gateway repo, or submit straight from the Amiga:

```
amipkg submit mytool http://your.host/files/MyTool.lha CAT=Utilities "Does a thing"
```
