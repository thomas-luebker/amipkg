## amipkg 0.7.0 — host your own repository

amipkg can now install from **more than one repository**, and anyone can run one.

A repository is just **two static files on any web server** — `packages.json` and
its signature. No database, no PHP, no HTTPS needed. A club, a developer shipping
their own builds, or you, for your own machines.

```
amipkg repo add mystuff http://your.host/amiga <public-key>
amipkg update
```

### What's new

**Multiple repositories.** Add as many as you like. The official catalog keeps
working exactly as before — if you never add one, nothing changes for you.

**Order is priority.** The first repository that has a package is the one you
get, and you decide the order with `amipkg repo up` / `down`. Want a specific
repo's build regardless? Ask for it by name:

```
amipkg install mystuff:ibrowse
```

**A repository manager in both GUIs.** Package → Repositories, in the GadTools
and the MUI front-end alike: add, remove, enable, disable, reorder. `amipkg info`
and both GUIs show which repository a package came from, so nothing is a mystery.

**Dependencies resolve across repositories.** A package in one repo can satisfy a
dependency declared in another — they behave as one catalog, not silos.

**Your own repo can be signed, and it's easy.** `tools/amipkg-repo-sign` makes a
keypair and signs your catalog with no dependencies at all — Python 3 and nothing
else. Your users then get exactly the same guarantee as the official repo: amipkg
verifies the signature **on the Amiga** before it will use a catalog, so even
over plain HTTP nobody between you and your users can alter what they install.

**Unsigned repos are allowed too**, if you want one — amipkg explains the
trade-off once and asks. It's a fair choice on your own LAN. It's a poor one for
strangers over the internet, because an unsigned catalog can be rewritten in
transit, and the archive checksums can't save you: they live inside the very
catalog that was rewritten.

New guide: **HOSTING.md** walks through both paths end to end.

### Open by design

amipkg is Apache-2.0 and the whole chain is inspectable: the client, the
catalog, the signing tool, the schema. Now the *repository* side is open too —
you no longer need anyone's permission to publish Amiga software to your own
users. If you'd rather reach everyone by default, the official catalog is still
one command away (`amipkg submit`), human-reviewed and signed offline.

### Compatibility

Nothing to migrate. Existing images keep their catalog exactly where it is and
keep updating from the official repo as before.
