/*
 * store.h - amipkg on-image data access (receipt DB + seeded index paths).
 *
 * The small I/O + parse helpers shared by BOTH front-ends: the CLI (main.c)
 * and the GadTools GUI (gui.c). Keeping them here means the two front-ends
 * read the receipt DB and index through ONE implementation and cannot drift.
 *
 * Portable C (fopen/fread); on the host build the AMIPKG: paths simply
 * don't resolve, which is fine - the host build is for logic testing.
 */
#ifndef AMIPKG_STORE_H
#define AMIPKG_STORE_H

#include <stddef.h>
#include "receipts.h"

/* amipkg's HOME is the drawer the binaries live in (like MUI:) - catalog,
 * cache and receipt DB sit right next to them. Data paths are built at
 * RUNTIME from amipkg_prefix():
 *   "PROGDIR:"  the normal case - the running binary's drawer IS the home
 *               (db/ or packages.json beside it, or a fresh standalone).
 *               NO assign is created, NOTHING is locked beyond execution.
 *   "AMIPKG:"   legacy fallback for pre-0.4.4 Amiga-Imager images where the
 *               CLI lives in C: and the data under the boot-time assign
 *               (created from AMIGAIMAGER: on 0.99-era images).
 * Use amipkg_data_path("db/installed.txt") for a ready-to-open path, or
 * printf-style "%sdb/files/%s.files", amipkg_prefix(), id. */
const char *amipkg_prefix(void);
char *amipkg_data_path(const char *rel);

/* THE version - single source for $VER tags, About boxes, the UA string
 * and the self-seeded receipt. Bump HERE (plus the catalog entry). */
#define AMIPKG_VERSION "0.7.0"
#define AMIPKG_VERDATE "28.7.2026"

#define AMIPKG_DEFAULT_INSTALLDIR "SYS:Programs"
/* 320: the signed catalog crossed 200 packages (2026-07-27) - 128 silently
 * truncated the GUI lists at 'n' (tester report). Every consumer uses the
 * ONE shared scratch below instead of per-function statics, so raising this
 * costs each binary one buffer, not fourteen. */
#define MAX_PKGS   320
#define MAX_FILES  512

/* Read a whole file into a malloc'd NUL-terminated buffer (caller frees).
 * NULL on any error. */
char *read_file(const char *path);

/* SHA-256 of a file as lowercase hex (out_hex must hold 65 bytes).
 * Returns 0 on success, nonzero if the file can't be read. */
int sha256_of_file(const char *path, char out_hex[65]);

/* Load the installed-packages receipt list (AMIPKG:db/installed.txt).
 * Returns the count (0 if absent/empty). */
size_t load_installed(rcpt_installed *out, size_t max);

/* Shared scratch for installed-receipt loads (BSS budget: 1 MB machines).
 * Safe because no holder calls another user mid-iteration (audited: upgrade
 * snapshots ids first; remove inlines its rewrite; the rest are leaves). */
extern rcpt_installed amipkg_inst_scratch[MAX_PKGS];

/* Load one package's recorded file inventory (files/<id>.files). */
size_t load_files_for(const char *id, rcpt_file *out, size_t max);

/* Resolve where generic (recipe-less) packages install to, in priority order:
 *   1. the AMIPKG:config/installdir file (set via GUI / `amipkg dir`)
 *   2. the AMIPKG_INSTALLDIR environment variable (setenv override)
 *   3. the "SYS:Programs" default.
 * Writes the trimmed path into out (size n). */
void amipkg_get_installdir(char *out, size_t n);

/* Persist the generic install dir to AMIPKG:config/installdir.
 * Returns 0 on success. An empty/NULL path clears it (reverts to default). */
int amipkg_set_installdir(const char *path);

/* Per-PACKAGE install-dir override (config/dir-<id>) - set by `amipkg adopt`
 * so upgrades land where the user already keeps the app. Falls back to the
 * global dir when absent. */
void amipkg_get_pkgdir(const char *id, char *out, size_t n);
int amipkg_set_pkgdir(const char *id, const char *path);

/* Warm up the data-prefix resolution (see amipkg_prefix; AssignLock from
 * whichever exists). No-op on the host build. Call once at startup -
 * the CLI (ensure_dirs) and BOTH GUIs do. */
void amipkg_bridge_assigns(void);

#ifndef __amigaos__
/* HOST BUILD ONLY. The resolved prefix is cached; the test suite repoints it
 * via AMIPKG_PREFIX and needs the cache dropped. Not compiled on the Amiga. */
void amipkg_reset_prefix_for_test(void);
#endif

#endif /* AMIPKG_STORE_H */
