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
 * printf-style "%sdb/files/%s.files", amipkg_prefix(), id.
 * The prefix is an ABSOLUTE path since 0.4.7 (not the short "AMIPKG:"), so
 * EVERY buffer built from it must be sized off AMIPKG_PREFIX_MAX - callers
 * included; -Wformat-truncation once caught three sized for the old short
 * form. Paths derived from it are also what belongs in a persisted receipt:
 * "PROGDIR:" resolves per-process and must never be written to one. */
#define AMIPKG_PREFIX_MAX 300
const char *amipkg_prefix(void);
char *amipkg_data_path(const char *rel);

/* THE version - single source for $VER tags, About boxes, the UA string
 * and the self-seeded receipt. Bump HERE (plus the catalog entry). */
#define AMIPKG_VERSION "0.7.9"
#define AMIPKG_VERDATE "4.8.2026"

/* Shown by BOTH GUIs when a shelled-out command produced NOTHING at all.
 * An empty output file does not mean "it failed quietly" - it means the
 * command never started, and on a 68k box that is nearly always memory: the
 * CLI has to load and hold the whole catalog (200+ packages is ~600 KB of
 * entries plus the JSON) on top of a resident Workbench, MUI and dock.
 * Reported 2026-07-31 in FS-UAE with 2.7 MB free, and it works with more RAM;
 * the emulator .uae asks for 256 MB Z3, but FS-UAE ignores z3mem_size and
 * needs `zorro_iii_memory` set in its OWN config. Same root cause as the
 * AmiSSL "5.x is required" reports. */
#define AMIPKG_NO_OUTPUT_HINT \
    "The command produced no output, which means it could not be\n" \
    "started at all - on 68k that is almost always free memory.\n\n" \
    "Close some programs and retry. Under an emulator, give the\n" \
    "machine more Fast RAM (FS-UAE: zorro_iii_memory)."

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

/* Which platform this binary is actually running on, in Aminet's vocabulary
 * ("m68k-amigaos", "ppc-morphos", "ppc-amigaos", "i386-aros"). amipkg is an
 * m68k binary but MorphOS and AmigaOS 4 both execute those, so it can find
 * itself somewhere other than a classic Amiga. Lives in the core because ALL
 * THREE front-ends need it - the GUIs do not link install.c. */
const char *amipkg_host_arch(void);

/* Is this path a SYSTEM drawer (C:, LIBS:, DEVS:, L:, S:, FONTS:, CLASSES:,
 * PREFS:), and which one? A recipe-less install targeting one of these must
 * place programs FLAT and put the rest beside them - copying a whole archive
 * tree into C: buries the command at C:Tool/Tool and drags the docs along
 * (tester reports, 2026-07-28). Returns SD_NONE for ordinary app drawers, so
 * their behaviour is unchanged. */
enum { SD_NONE = 0, SD_C, SD_LIBS, SD_DEVS, SD_L, SD_S, SD_FONTS, SD_CLASSES, SD_PREFS };
int amipkg_system_drawer_kind(const char *path);

#ifndef __amigaos__
/* HOST BUILD ONLY. The resolved prefix is cached; the test suite repoints it
 * via AMIPKG_PREFIX and needs the cache dropped. Not compiled on the Amiga. */
void amipkg_reset_prefix_for_test(void);
#endif

#endif /* AMIPKG_STORE_H */
