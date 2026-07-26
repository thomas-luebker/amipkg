# amipkg 0.3 — hardened, cleaner, closer to a real system tool

Hardware-validated on a real A4000/68060. Upgrading from 0.2: replace
`C:amipkg` and `SYS:Tools/amipkg-gui` (or just run `amipkg upgrade amipkg`).

## New in 0.3

- **Pre/post-install scripts**: recipes can now carry reviewed, inline
  AmigaDOS lines that run before/after installation (assigns, ENVARC
  defaults, tidy-ups) — curated into the signed catalog like everything else.
- **Truly clean removal**: `amipkg remove` now strips the package's
  `S:User-Startup` blocks, and recipes can ship an uninstall script that is
  saved into the receipt at install time and run at removal.
- **Download resume**: interrupted transfers continue where they stopped
  (HTTP Range), including across mirrors — a blessing on slow links.
- **`amipkg doctor`**: audits every installed package against its receipt
  (missing/modified files) and tells you what to reinstall.
- **`amipkg install <id> DRYRUN`**: preview the resolved dependency plan and
  download sizes without changing anything. The GUI now shows this plan in
  the install confirmation.
- **CPU/Kickstart floors**: packages that need a better CPU or newer
  Kickstart are refused with a clear message instead of failing obscurely.
- `Version C:amipkg` reports the exact build (`$VER` tags).

## Assets

- **`amipkg.lha`** — full bundle: CLI + GUI + signed catalog (offline
  browsing) + double-clickable Install. Start here.
- **`amipkg-client.lha`** — binaries only; what self-update fetches.

Catalog + submissions: https://github.com/thomas-luebker/amiga-pkg
