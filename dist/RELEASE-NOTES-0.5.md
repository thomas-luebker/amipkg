# amipkg 0.5.0 (27.7.2026)

The release after 0.4.9/0.4.10 jumps to 0.5.0 — the MUI front-end grew up.

## New

- **Redesigned MUI GUI** (inspired by DJBase's LumiPass layout — thanks!):
  - Toolbar with grouped actions (catalog ops | package ops | refresh)
  - Sidebar with **View** (Installed/Available) and **Categories** panes —
    click to filter, no more cycling through 12 categories
  - Balance bar between sidebar and list — drag to taste, MUI snapshots it
  - Framed description pane, search + sort row above the list
  - Status bar with live counters: `Catalog | Installed | Shown`
  - Window title shows the running version
  - Stock MUI 3.8 classes only — still zero .mcc dependencies
- Catalog crossed **200 signed packages**, including DJBase's native
  Fallout 1 CE and Fallout 2 CE 68k engine ports (amigaworld.de).

## Carried over from the 0.4.x line

- Lives fully in its own drawer (no assigns, no Startup-Sequence writes);
  optional Install-System integration
- `adopt`, self-update via the catalog, Ed25519-verified index updates
- GadTools GUI feature parity (View/Categories remain cycle gadgets there —
  it must run on a stock 68000 with zero dependencies)

## Notes

- The GadTools front-end intentionally keeps its simpler layout; all
  *features* stay in lockstep with the MUI front-end.
