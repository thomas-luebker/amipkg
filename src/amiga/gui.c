/*
 * gui.c — amipkg-gui, a GadTools front-end for the AmigaImager package manager.
 *
 * A Workbench GUI for technical users over the same on-image data the CLI reads
 * (the receipt DB + seeded index via src/core/store.c). Reads are structured
 * (the listview is populated from load_installed / the parsed index); the
 * mutating actions (install / remove) shell out to the amipkg CLI so the validated
 * CLI logic is the single source of truth and cannot drift.
 *
 * Toolkit: GadTools (gadtools.library over Intuition) — core OS since 2.0, so
 * this runs on every AmigaImager-built image (3.0-3.9) with no MUI/ReAction
 * dependency. v37+ (Kickstart 2.04) required, which every target exceeds.
 *
 * A "View" cycle switches the listview between INSTALLED packages (from the
 * receipt DB) and AVAILABLE packages (the whole seeded index — what the repo
 * offers). Buttons act on the selection: Info always; Install in the Available
 * view; Remove in the Installed view. Results land in a status line at the foot
 * of the window (requesters only for detail or confirmation).
 *
 * PARITY RULE: gui.c and mui.c (the MUI front-end) are maintained in
 * LOCKSTEP — every user-facing feature lands in BOTH in the same change.
 */

#ifdef __amigaos__

#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <exec/lists.h>
#include <exec/nodes.h>

#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <proto/asl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/store.h"
#include "../core/aindex.h"
#include "../core/aver.h"

#define AMIPKG_GUI_VERSION "amipkg-gui 0.4"

/* AmigaOS Version-command tag: `Version SYS:Tools/amipkg-gui` reports the
 * exact build — essential for tester feedback. `used` keeps -Os from
 * discarding the unreferenced constant. */
static const char verstag[] __attribute__((used)) = "$VER: amipkg-gui 0.4 (25.7.2026)";

struct IntuitionBase *IntuitionBase = NULL;
struct Library       *GadToolsBase  = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *AslBase       = NULL;   /* optional: drawer requester */

static void init_list(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type     = 0;
}

/* Gadget IDs */
enum { GID_VIEW = 1, GID_LIST, GID_CHECK, GID_INFO, GID_INSTALL, GID_REMOVE, GID_REFRESH,
       GID_STATUS, GID_SEARCH, GID_UPGRADE, GID_PROGRESS, GID_UPDATE, GID_RUN,
       GID_CAT, GID_SORT };

/* Menu numbering (must match g_newmenu below). */
enum { MENU_PROJECT = 0, MENU_PACKAGE = 1 };
enum { PROJ_ABOUT = 0, PROJ_DOCS = 1, PROJ_QUIT = 3 };  /* item 2 is the bar */
enum { PKG_UPDATECAT = 0, PKG_CHECK = 1, PKG_UPGRADE = 2, PKG_INFO = 3, PKG_INSTALL = 4,
       PKG_ADOPT = 5, PKG_REMOVE = 6, PKG_REFRESH = 7, PKG_SETDIR = 9 };   /* item 8 = bar */

/* View modes. */
enum { VIEW_INSTALLED = 0, VIEW_AVAILABLE = 1 };
static const char *g_viewlabels[] = { "Installed", "Available", NULL };
static int g_view = VIEW_INSTALLED;

/* The list model, rebuilt from the receipt DB or the index per view. Each row
 * carries its package id so actions don't depend on which view built it. */
static struct List g_list;
static char        g_labels[MAX_PKGS][80];
static char        g_rowid[MAX_PKGS][64];
static char        g_rowdesc[MAX_PKGS][100];   /* catalog description per row */
static struct Node g_nodes[MAX_PKGS];
static size_t      g_nrows = 0;
static int         g_selected = -1;

static struct Gadget *g_glist = NULL;
static struct Gadget *g_gads[GID_SORT + 1];
static char           g_filter[64] = "";   /* Find box: case-insensitive id/name substring */
static char           g_progressbuf[160] = "";   /* last line of the running op's output */

/* Category filter (Available view): labels built from the index at refresh. */
#define MAX_CATS 16
static char        g_catnames[MAX_CATS][24];
static const char *g_catlabels[MAX_CATS + 2];   /* "All", cats..., NULL */
static size_t      g_ncats = 0;
static int         g_cat_sel = 0;               /* 0 = All */

/* Sort mode (Available view). */
static const char *g_sortlabels[] = { "By Name", "Newest", NULL };
static int g_sort_recent = 0;

/* Async operation state: one C:amipkg command runs detached via Execute of a
 * RAM: script; INTUITICKS polls its output into the progress line and its
 * completion sentinel for the return code. */
static int  g_busy = 0;
static char g_busy_verb[48];      /* "Install of 'foo'" — for the finish message */
static long g_busy_ticks = 0;     /* watchdog: ~10 ticks/s while window active */
#define ASYNC_OUT    "RAM:amipkg-gui.out"
#define ASYNC_DONE   "RAM:amipkg-gui.done"
#define ASYNC_SCRIPT "RAM:amipkg-gui.script"
static struct Window *g_win = NULL;
static struct Menu   *g_menu = NULL;
static void          *g_vi = NULL;
static struct Screen *g_scr = NULL;
static char           g_statusbuf[160];

static struct NewMenu g_newmenu[] = {
    { NM_TITLE, (STRPTR)"Project",       NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...",      NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Documentation...", (STRPTR)"?", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,     NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",          (STRPTR)"Q", 0, 0, NULL },
    { NM_TITLE, (STRPTR)"Package",       NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Update Catalog", (STRPTR)"A", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Check Updates", (STRPTR)"C", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Update All",    (STRPTR)"U", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Info",          (STRPTR)"I", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Install",       (STRPTR)"N", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Adopt Existing...", NULL,    0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Remove",        (STRPTR)"R", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Refresh",       (STRPTR)"F", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,     NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Install Drawer...", (STRPTR)"D", 0, 0, NULL },
    { NM_END,   NULL,                    NULL,        0, 0, NULL },
};

/* ---- helpers -------------------------------------------------------------- */

static int id_installed(const rcpt_installed *inst, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(inst[i].id, id) == 0) return 1;
    return 0;
}

/* Case-insensitive substring: does `hay` contain `needle`? Empty needle matches. */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (nl == 0) return 1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            int a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (!hay[i + j] || a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}


/* The CLI binary to shell out to. Standalone home first (AMIPKG:amipkg --
 * amipkg lives in the drawer it was unpacked into), then the Amiga-Imager
 * image location (C:amipkg). Resolved once, after the assign bridge ran. */
static const char *cli_path(void)
{
    static char path[16] = "";
    if (!path[0]) {
        BPTR l = Lock((STRPTR)"AMIPKG:amipkg", ACCESS_READ);
        if (l) { UnLock(l); strcpy(path, "AMIPKG:amipkg"); }
        else strcpy(path, "C:amipkg");
    }
    return path;
}

static void set_status(const char *s)
{
    strncpy(g_statusbuf, s, sizeof g_statusbuf - 1);
    g_statusbuf[sizeof g_statusbuf - 1] = '\0';
    if (g_win && g_gads[GID_STATUS])
        GT_SetGadgetAttrs(g_gads[GID_STATUS], g_win, NULL, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
}

/* The progress line under the status: shows the last output line of the running
 * C:amipkg operation (fetch/verify/install result). */
static void set_progress(const char *s)
{
    strncpy(g_progressbuf, s, sizeof g_progressbuf - 1);
    g_progressbuf[sizeof g_progressbuf - 1] = '\0';
    if (g_win && g_gads[GID_PROGRESS])
        GT_SetGadgetAttrs(g_gads[GID_PROGRESS], g_win, NULL, GTTX_Text, (ULONG)g_progressbuf, TAG_END);
}

static void update_action_state(void);   /* defined below */
static void action_refresh_after_op(void);

/* Read the LAST non-empty line of `path` into out (empty string when none). */
static void tail_line(const char *path, char *out, size_t outsize)
{
    static char buf[4096];
    BPTR fh;
    LONG n = 0;
    out[0] = '\0';
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return;
    /* Read the final chunk: seek to at most 4 KB before EOF. */
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_CURRENT);
    if (n > (LONG)sizeof buf - 1) { Seek(fh, -(LONG)(sizeof buf - 1), OFFSET_END); }
    else                          { Seek(fh, 0, OFFSET_BEGINNING); }
    n = Read(fh, buf, sizeof buf - 1);
    Close(fh);
    if (n <= 0) return;
    buf[n] = '\0';
    {
        char *last = buf, *p;
        for (p = buf; *p; p++)
            if (*p == '\n') { *p = '\0'; if (*(p + 1)) last = p + 1; }
        strncpy(out, last, outsize - 1);
        out[outsize - 1] = '\0';
    }
}

/* Launch a C:amipkg command DETACHED so the window stays alive: a RAM: script
 * runs the command with output to ASYNC_OUT, then writes its return code to
 * ASYNC_DONE. INTUITICKS (poll_async) stream the output tail into the progress
 * line and detect completion. Buttons stay disabled while g_busy. */
static int run_async(const char *cmd, const char *verb)
{
    BPTR fh, in, out;
    static char script[512];
    if (g_busy) { set_status("An operation is already running."); return 0; }
    DeleteFile((STRPTR)ASYNC_OUT);
    DeleteFile((STRPTR)ASYNC_DONE);
    snprintf(script, sizeof script,
             "FailAt 30\n%s >" ASYNC_OUT "\nEcho $RC >" ASYNC_DONE "\n", cmd);
    fh = Open((STRPTR)ASYNC_SCRIPT, MODE_NEWFILE);
    if (!fh) { set_status("Could not write the RAM: work script."); return 0; }
    Write(fh, script, (LONG)strlen(script));
    Close(fh);
    in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((STRPTR)"Execute " ASYNC_SCRIPT,
                   SYS_Asynch, TRUE, SYS_Input, in, SYS_Output, out,
                   TAG_END) == -1) {
        if (in) Close(in);
        if (out) Close(out);
        set_status("Could not launch the amipkg CLI.");
        return 0;
    }
    /* On success the system owns + closes the handles. */
    strncpy(g_busy_verb, verb, sizeof g_busy_verb - 1);
    g_busy_verb[sizeof g_busy_verb - 1] = '\0';
    g_busy = 1;
    g_busy_ticks = 0;
    set_progress("Working...");
    update_action_state();
    return 1;
}

/* One INTUITICKS heartbeat while an async op runs. */
static void poll_async(void)
{
    char line[160];
    BPTR fh;
    if (!g_busy) return;
    g_busy_ticks++;
    /* Stream progress ~2x/s (ticks fire ~10x/s). */
    if ((g_busy_ticks % 5) == 0) {
        tail_line(ASYNC_OUT, line, sizeof line);
        if (line[0]) set_progress(line);
    }
    fh = Open((STRPTR)ASYNC_DONE, MODE_OLDFILE);
    if (fh) {
        char rcbuf[16];
        LONG n = Read(fh, rcbuf, sizeof rcbuf - 1);
        long rc = 0;
        Close(fh);
        if (n > 0) { rcbuf[n] = '\0'; rc = atol(rcbuf); }
        tail_line(ASYNC_OUT, line, sizeof line);
        if (line[0]) set_progress(line);
        g_busy = 0;
        DeleteFile((STRPTR)ASYNC_DONE);
        DeleteFile((STRPTR)ASYNC_SCRIPT);
        {
            char st[96];
            if (rc == 0) snprintf(st, sizeof st, "%s finished.", g_busy_verb);
            else         snprintf(st, sizeof st, "%s FAILED (rc %ld) - see the line below.", g_busy_verb, rc);
            action_refresh_after_op();
            set_status(st);
        }
        update_action_state();
    } else if (g_busy_ticks > 10L * 60 * 30) {   /* ~30 min watchdog */
        g_busy = 0;
        set_status("Operation timed out - check a Shell.");
        update_action_state();
    }
}

static void status_count(void)
{
    char b[64];
    snprintf(b, sizeof b, "%lu package%s %s",
             (unsigned long)g_nrows, g_nrows == 1 ? "" : "s",
             g_view == VIEW_AVAILABLE ? "available" : "installed");
    set_status(b);
}

static int selection_installed(void);   /* defined below */

/* Info: any selection. Install: Available view + selection. Remove/Run:
 * installed selection. Everything mutating is locked while an async op runs. */
static void update_action_state(void)
{
    int sel = (g_selected >= 0 && (size_t)g_selected < g_nrows);
    int busy = g_busy;
    int inst = sel && selection_installed();
    if (g_gads[GID_INFO])
        GT_SetGadgetAttrs(g_gads[GID_INFO], g_win, NULL, GA_Disabled, sel ? FALSE : TRUE, TAG_END);
    if (g_gads[GID_INSTALL])
        GT_SetGadgetAttrs(g_gads[GID_INSTALL], g_win, NULL,
                          GA_Disabled, (!busy && sel && g_view == VIEW_AVAILABLE) ? FALSE : TRUE, TAG_END);
    if (g_gads[GID_REMOVE])
        GT_SetGadgetAttrs(g_gads[GID_REMOVE], g_win, NULL,
                          GA_Disabled, (!busy && inst) ? FALSE : TRUE, TAG_END);
    if (g_gads[GID_RUN])
        GT_SetGadgetAttrs(g_gads[GID_RUN], g_win, NULL,
                          GA_Disabled, (!busy && inst) ? FALSE : TRUE, TAG_END);
    if (g_gads[GID_UPDATE])
        GT_SetGadgetAttrs(g_gads[GID_UPDATE], g_win, NULL, GA_Disabled, busy ? TRUE : FALSE, TAG_END);
    if (g_gads[GID_UPGRADE])
        GT_SetGadgetAttrs(g_gads[GID_UPGRADE], g_win, NULL, GA_Disabled, busy ? TRUE : FALSE, TAG_END);
    if (g_gads[GID_REFRESH])
        GT_SetGadgetAttrs(g_gads[GID_REFRESH], g_win, NULL, GA_Disabled, busy ? TRUE : FALSE, TAG_END);
}

/* ---- model ---------------------------------------------------------------- */

static void add_row(const char *label, const char *id, const char *desc)
{
    if (g_nrows >= MAX_PKGS) return;
    strncpy(g_labels[g_nrows], label, sizeof g_labels[0] - 1);
    g_labels[g_nrows][sizeof g_labels[0] - 1] = '\0';
    strncpy(g_rowid[g_nrows], id, sizeof g_rowid[0] - 1);
    g_rowid[g_nrows][sizeof g_rowid[0] - 1] = '\0';
    strncpy(g_rowdesc[g_nrows], desc ? desc : "", sizeof g_rowdesc[0] - 1);
    g_rowdesc[g_nrows][sizeof g_rowdesc[0] - 1] = '\0';
    g_nodes[g_nrows].ln_Name = g_labels[g_nrows];
    AddTail(&g_list, &g_nodes[g_nrows]);
    g_nrows++;
}

static const char *shown_version(const char *v)
{
    return (!v[0] || (v[0] == '-' && !v[1])) ? "-" : v;
}

/* Register a category in the filter cycle's label set (dedup, cap MAX_CATS). */
static void note_category(const char *cat)
{
    size_t i;
    if (!cat[0]) return;
    for (i = 0; i < g_ncats; i++)
        if (strcmp(g_catnames[i], cat) == 0) return;
    if (g_ncats >= MAX_CATS) return;
    strncpy(g_catnames[g_ncats], cat, sizeof g_catnames[0] - 1);
    g_catnames[g_ncats][sizeof g_catnames[0] - 1] = '\0';
    g_ncats++;
}

/* "Newest first" comparator: added dates are ISO YYYY-MM-DD so strcmp orders
 * them; entries without a date sort last (then by id). */
static int cmp_recent(const void *a, const void *b)
{
    const aidx_entry *ea = *(const aidx_entry * const *)a;
    const aidx_entry *eb = *(const aidx_entry * const *)b;
    int ha = ea->added[0] != '\0', hb = eb->added[0] != '\0';
    if (ha != hb) return hb - ha;
    if (ha) {
        int c = strcmp(eb->added, ea->added);
        if (c) return c;
    }
    return strcmp(ea->id, eb->id);
}

static void rebuild_list(void)
{
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst, i;
    char label[80];
    init_list(&g_list);
    g_nrows = 0;
    ninst = load_installed(inst, MAX_PKGS);

    if (g_view == VIEW_INSTALLED) {
        for (i = 0; i < ninst; i++) {
            if (!contains_ci(inst[i].id, g_filter)) continue;
            snprintf(label, sizeof label, "%-22s %s", inst[i].id, shown_version(inst[i].version));
            add_row(label, inst[i].id, NULL);
        }
        /* First run opens in this view: still harvest the category labels so
         * the filter cycle is populated before the first switch to Available. */
        if (g_ncats == 0) {
            aidx_index idx;
            char *text = read_file(AMIPKG_INDEX_PATH);
            if (text && aidx_parse(text, &idx) == 0) {
                for (i = 0; i < idx.count; i++) note_category(idx.entries[i].category);
                aidx_free(&idx);
            }
            if (text) free(text);
        }
    } else {
        aidx_index idx;
        char *text = read_file(AMIPKG_INDEX_PATH);
        if (text && aidx_parse(text, &idx) == 0) {
            static const aidx_entry *order[MAX_PKGS * 2];
            size_t norder = 0;
            const char *want_cat = g_cat_sel > 0 && (size_t)(g_cat_sel - 1) < g_ncats
                                   ? g_catnames[g_cat_sel - 1] : NULL;
            /* Categories refresh from the FULL index (unfiltered). */
            g_ncats = 0;
            for (i = 0; i < idx.count; i++) note_category(idx.entries[i].category);
            /* Filter (Find + category), then order. */
            for (i = 0; i < idx.count && norder < sizeof order / sizeof order[0]; i++) {
                const aidx_entry *e = &idx.entries[i];
                if (!contains_ci(e->id, g_filter) && !contains_ci(e->name, g_filter)) continue;
                if (want_cat && strcmp(e->category, want_cat) != 0) continue;
                order[norder++] = e;
            }
            if (g_sort_recent)
                qsort(order, norder, sizeof order[0], cmp_recent);
            for (i = 0; i < norder; i++) {
                const aidx_entry *e = order[i];
                /* Leading marker so installed state is scannable at a glance. */
                snprintf(label, sizeof label, "%s %-18s %-8s %s",
                         id_installed(inst, ninst, e->id) ? "*" : " ",
                         e->id, shown_version(e->version),
                         id_installed(inst, ninst, e->id) ? "installed" : "");
                add_row(label, e->id, e->description);
            }
            aidx_free(&idx);
        } else {
            set_status("No catalog yet - click Update Catalog (needs network).");
        }
        if (text) free(text);
    }
}

/* Is the currently-selected row an installed package (receipt DB)? */
static int selection_installed(void)
{
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst;
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) return 0;
    ninst = load_installed(inst, MAX_PKGS);
    return id_installed(inst, ninst, g_rowid[g_selected]);
}

/* ---- requesters ----------------------------------------------------------- */

static void req(const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof es; es.es_Flags = 0;
    es.es_Title = (UBYTE *)title; es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(g_win, &es, NULL);
}

static int req_confirm(const char *title, const char *body, const char *gadgets)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof es; es.es_Flags = 0;
    es.es_Title = (UBYTE *)title; es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)gadgets;
    return EasyRequest(g_win, &es, NULL) == 1;
}

/* ---- actions -------------------------------------------------------------- */

static void action_about(void)
{
    req("About",
        AMIPKG_GUI_VERSION "\n\n"
        "The AmigaPKG package manager for AmigaOS 3.x.\n"
        "Browse, install, update, and remove software\n"
        "from the signed AmigaPKG catalog.\n\n"
        "(c) 2026 Thomas Luebker\n\n"
        "Catalog + package submissions:\n"
        "https://github.com/thomas-luebker/amiga-pkg");
}

static void action_refresh(void);   /* defined below */

/* "Update Catalog" — fetch the latest signed index from the repo, Ed25519-verify
 * it on-device, replace the local catalog, and reload the list. Shells to
 * `C:amipkg update` (needs a TCP/IP stack up). */
static void action_update_catalog(void)
{
    set_status("Updating catalog... (needs a TCP/IP stack up)");
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s update", cli_path());
    run_async(cmd, "Catalog update");
}

static void action_check(void)
{
    static aidx_index idx;
    char *text = read_file(AMIPKG_INDEX_PATH);
    char msg[2048];
    size_t used = 0, i;
    int updates = 0;
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst;
    set_status("Checking for updates...");
    if (!text) { set_status("No catalog yet - click Update Catalog (needs network)."); return; }
    if (aidx_parse(text, &idx) != 0) { free(text); set_status("The seeded index is unreadable."); return; }
    free(text);
    ninst = load_installed(inst, MAX_PKGS);
    used += (size_t)snprintf(msg + used, sizeof msg - used, "Updates available:\n\n");
    for (i = 0; i < ninst && used < sizeof msg - 64; i++) {
        const aidx_entry *e = aidx_find(&idx, inst[i].id);
        if (!e) continue;
        if (aver_is_newer(aidx_comparable_version(e), inst[i].version)) {
            used += (size_t)snprintf(msg + used, sizeof msg - used,
                                     "  %-20s %s -> %s\n", inst[i].id, inst[i].version, e->version);
            updates++;
        }
    }
    aidx_free(&idx);
    if (updates == 0) {
        set_status("Everything is up to date.");
    } else {
        char st[64];
        snprintf(st, sizeof st, "%d update%s available.", updates, updates == 1 ? "" : "s");
        set_status(st);
        req("Check Updates", msg);
    }
}

/* "Update All" — upgrade every out-of-date package via `C:amipkg upgrade`.
 * Each upgrade removes the old version's files first, then installs the new. */
static void action_upgrade(void)
{
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!req_confirm("Update All",
                     "Upgrade every out-of-date package?\n\nDownloads + reinstalls the newer\n"
                     "versions (needs a TCP/IP stack up).", "Update|Cancel")) {
        set_status("Update cancelled."); return;
    }
    set_status("Updating all packages...");
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s upgrade", cli_path());
    run_async(cmd, "Update All");
}

static void action_info(void)
{
    aidx_index idx;
    const aidx_entry *e;
    char *text;
    char msg[1024];
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst, k;
    const char *id, *insver = "(not installed)";
    int ins;
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    id = g_rowid[g_selected];
    text = read_file(AMIPKG_INDEX_PATH);
    if (!text) { set_status("No catalog yet - click Update Catalog (needs network)."); return; }
    if (aidx_parse(text, &idx) != 0) { free(text); set_status("The seeded index is unreadable."); return; }
    free(text);
    ninst = load_installed(inst, MAX_PKGS);
    ins = id_installed(inst, ninst, id);
    for (k = 0; k < ninst; k++)
        if (strcmp(inst[k].id, id) == 0) { insver = shown_version(inst[k].version); break; }
    e = aidx_find(&idx, id);
    if (!e) {
        snprintf(msg, sizeof msg, "%s\n\ninstalled: %s\n(not in the seeded index)", id, ins ? insver : "no");
    } else {
        size_t u = 0, k;
        u += (size_t)snprintf(msg + u, sizeof msg - u, "%s - %s\n", e->id, e->name);
        if (e->description[0])
            u += (size_t)snprintf(msg + u, sizeof msg - u, "\n%s\n\n", e->description);
        u += (size_t)snprintf(msg + u, sizeof msg - u, "category: %s\n", e->category);
        u += (size_t)snprintf(msg + u, sizeof msg - u, "available: %s\n", e->version);
        if (e->added[0])
            u += (size_t)snprintf(msg + u, sizeof msg - u, "added: %s\n", e->added);
        u += (size_t)snprintf(msg + u, sizeof msg - u, "installed: %s\n", ins ? insver : "(not installed)");
        if (e->dep_count) {
            u += (size_t)snprintf(msg + u, sizeof msg - u, "needs:");
            for (k = 0; k < e->dep_count && u < sizeof msg - 70; k++)
                u += (size_t)snprintf(msg + u, sizeof msg - u, " %s", e->deps[k].id);
            u += (size_t)snprintf(msg + u, sizeof msg - u, "\n");
        }
        u += (size_t)snprintf(msg + u, sizeof msg - u, "install: %s",
                              e->has_recipe ? "portable recipe" : "build-time only");
    }
    aidx_free(&idx);
    req("Package Info", msg);
}

static void list_detach(void)
{ GT_SetGadgetAttrs(g_gads[GID_LIST], g_win, NULL, GTLV_Labels, ~0L, TAG_END); }
static void list_reattach(void)
{ GT_SetGadgetAttrs(g_gads[GID_LIST], g_win, NULL, GTLV_Labels, (ULONG)&g_list, TAG_END); }

static void action_refresh(void)
{
    list_detach();
    rebuild_list();
    g_selected = -1;
    list_reattach();
    update_action_state();
    status_count();
}

static void switch_view(int v)
{
    if (v == g_view) return;
    g_view = v;
    action_refresh();
}

/* Find box changed (Enter or deactivate): copy the buffer into g_filter and
 * re-filter the list. */
static void action_search(struct Gadget *g)
{
    struct StringInfo *si = (struct StringInfo *)g->SpecialInfo;
    char b[80];
    if (!si || !si->Buffer) return;
    strncpy(g_filter, (const char *)si->Buffer, sizeof g_filter - 1);
    g_filter[sizeof g_filter - 1] = '\0';
    list_detach();
    rebuild_list();
    g_selected = -1;
    list_reattach();
    update_action_state();
    if (g_filter[0])
        snprintf(b, sizeof b, "%lu match%s for \"%s\"",
                 (unsigned long)g_nrows, g_nrows == 1 ? "" : "es", g_filter);
    else
        snprintf(b, sizeof b, "%lu package%s %s", (unsigned long)g_nrows,
                 g_nrows == 1 ? "" : "s", g_view == VIEW_AVAILABLE ? "available" : "installed");
    set_status(b);
}

/* "Adopt Existing..." — the selected catalog package is ALREADY on this
 * system somewhere: pick its drawer, and amipkg takes over managing it
 * (inventory + receipt + in-place upgrades). jdb78's idea. */
static void action_adopt(void)
{
    struct FileRequester *fr;
    char cmd[400], verb[48];
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select the package you already have."); return; }
    if (!AslBase) { set_status("asl.library unavailable - use: amipkg adopt <id> <drawer>"); return; }
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,   (ULONG)"Where is it installed? (pick its drawer)",
            ASLFR_DrawersOnly, TRUE,
            TAG_END);
    if (!fr) { set_status("Could not open the drawer requester."); return; }
    if (AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        snprintf(cmd, sizeof cmd, "%s adopt %s \"%s\"", cli_path(), g_rowid[g_selected], fr->fr_Drawer);
        snprintf(verb, sizeof verb, "Adopt of '%s'", g_rowid[g_selected]);
        set_status("Adopting...");
        run_async(cmd, verb);
    }
    FreeAslRequest(fr);
}

/* "Install Drawer..." — pick where recipe-less packages install to, via the ASL
 * drawer requester, and persist it (shared with the CLI's `amipkg dir`). */
static void action_set_dir(void)
{
    struct FileRequester *fr;
    char cur[256], b[300];
    if (!AslBase) { set_status("asl.library unavailable — use: amipkg dir <path>"); return; }
    amipkg_get_installdir(cur, sizeof cur);
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select install drawer",
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_InitialDrawer, (ULONG)cur,
            TAG_END);
    if (!fr) { set_status("Could not open the drawer requester."); return; }
    if (AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        if (amipkg_set_installdir(fr->fr_Drawer) == 0) {
            snprintf(b, sizeof b, "Install drawer: %s", fr->fr_Drawer);
            set_status(b);
        } else {
            set_status("Could not save the install drawer.");
        }
    }
    FreeAslRequest(fr);
}

static void action_remove(void)
{
    char cmd[256], body[256], verb[48];
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    snprintf(body, sizeof body,
             "Remove '%s'?\n\nFiles modified since install are kept;\nshared files are never deleted.",
             g_rowid[g_selected]);
    if (!req_confirm("Remove Package", body, "Remove|Cancel")) { set_status("Remove cancelled."); return; }
    snprintf(body, sizeof body, "Removing %s...", g_rowid[g_selected]);
    set_status(body);
    snprintf(cmd, sizeof cmd, "%s remove %s FORCE", cli_path(), g_rowid[g_selected]);
    snprintf(verb, sizeof verb, "Remove of '%s'", g_rowid[g_selected]);
    run_async(cmd, verb);
}

/* Synchronously capture a quick command's full output (DRYRUN: local JSON
 * work only, no network) into buf. Returns the byte count. */
static long run_sync_capture(const char *cmd, char *buf, long bufsize)
{
    char full[300];
    BPTR fh;
    LONG n = 0;
    snprintf(full, sizeof full, "%s >RAM:amipkg-plan.out", cmd);
    SystemTags(full, TAG_DONE);
    fh = Open((STRPTR)"RAM:amipkg-plan.out", MODE_OLDFILE);
    if (fh) { n = Read(fh, buf, bufsize - 1); Close(fh); }
    DeleteFile((STRPTR)"RAM:amipkg-plan.out");
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

static void action_install(void)
{
    char cmd[256], id[64], verb[48];
    static char plan[1200], body[1500];
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    strncpy(id, g_rowid[g_selected], sizeof id - 1); id[sizeof id - 1] = '\0';

    /* Show the REAL resolved plan (deps + download sizes) in the confirm:
     * `install <id> DRYRUN` is local-only and fast. */
    set_status("Resolving...");
    snprintf(cmd, sizeof cmd, "%s install %s DRYRUN", cli_path(), id);
    run_sync_capture(cmd, plan, sizeof plan);
    if (plan[0])
        snprintf(body, sizeof body,
                 "Install '%s'?\n\n%s\nDownloads are SHA-256-verified.\nNeeds a TCP/IP stack up.",
                 id, plan);
    else
        snprintf(body, sizeof body,
                 "Install '%s'?\n\nDownloads + verifies the archive and\ninstalls it (with any dependencies).\nNeeds a TCP/IP stack up.",
                 id);
    if (!req_confirm("Install Package", body, "Install|Cancel")) { set_status("Install cancelled."); return; }
    snprintf(body, sizeof body, "Installing %s...", id);
    set_status(body);
    snprintf(cmd, sizeof cmd, "%s install %s", cli_path(), id);
    snprintf(verb, sizeof verb, "Install of '%s'", id);
    run_async(cmd, verb);
}

/* "Run" — launch the selected installed package. The executable is found from
 * its receipt file list: prefer a file whose name matches the package id,
 * else the largest AmigaDOS hunk executable it installed. Launched via a RAM:
 * script that first CDs into the program's drawer (assets/config live there). */
static void action_run(void)
{
    static rcpt_file files[MAX_FILES];
    size_t nfiles, i;
    const char *id;
    char best[256] = "", bestname[256] = "";
    long best_size = -1;
    int best_is_name_match = 0;

    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    id = g_rowid[g_selected];
    nfiles = load_files_for(id, files, MAX_FILES);
    if (nfiles == 0) { set_status("No file inventory for this package - run it from Workbench."); return; }

    for (i = 0; i < nfiles; i++) {
        const char *path = files[i].path;
        const char *base = strrchr(path, '/');
        FILE *f;
        unsigned char magic[4];
        long size = 0;
        int name_match;
        base = base ? base + 1 : (strrchr(path, ':') ? strrchr(path, ':') + 1 : path);
        /* Skip icons and obvious non-programs cheaply. */
        if (strlen(base) > 5 && strcmp(base + strlen(base) - 5, ".info") == 0) continue;
        f = fopen(path, "rb");
        if (!f) continue;
        if (fread(magic, 1, 4, f) != 4
            || magic[0] != 0 || magic[1] != 0 || magic[2] != 3 || magic[3] != 0xF3) {
            fclose(f); continue;   /* not an AmigaDOS hunk executable */
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
        name_match = strncasecmp(base, id, strlen(id)) == 0;
        if ((name_match && !best_is_name_match)
            || (name_match == best_is_name_match && size > best_size)) {
            strncpy(best, path, sizeof best - 1); best[sizeof best - 1] = '\0';
            strncpy(bestname, base, sizeof bestname - 1); bestname[sizeof bestname - 1] = '\0';
            best_size = size;
            best_is_name_match = name_match;
        }
    }
    if (!best[0]) { set_status("No executable found in this package's files."); return; }

    {
        /* CD into the program's drawer, then Run it detached. */
        char drawer[256], b[300];
        BPTR fh;
        static char script[600];
        size_t dl = (size_t)(strlen(best) - strlen(bestname));
        if (dl >= sizeof drawer) dl = sizeof drawer - 1;
        memcpy(drawer, best, dl); drawer[dl] = '\0';
        snprintf(script, sizeof script, "CD \"%s\"\nRun >NIL: \"%s\"\n", drawer, bestname);
        fh = Open((STRPTR)"RAM:amipkg-runapp.script", MODE_NEWFILE);
        if (!fh) { set_status("Could not write the RAM: launch script."); return; }
        Write(fh, script, (LONG)strlen(script));
        Close(fh);
        {
            BPTR in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
            BPTR out = Open((STRPTR)"NIL:", MODE_NEWFILE);
            if (SystemTags((STRPTR)"Execute RAM:amipkg-runapp.script",
                           SYS_Asynch, TRUE, SYS_Input, in, SYS_Output, out,
                           TAG_END) == -1) {
                if (in) Close(in);
                if (out) Close(out);
                set_status("Could not launch the program.");
                return;
            }
        }
        snprintf(b, sizeof b, "Launched %s.", bestname);
        set_status(b);
    }
}

/* Post-async-op list reload (poll_async calls this; keeps the status line for
 * the finish message, unlike action_refresh which overwrites it). */
static void action_refresh_after_op(void)
{
    list_detach();
    rebuild_list();
    g_selected = -1;
    list_reattach();
}

/* ---- UI construction ------------------------------------------------------ */

#define UI_MARGIN 8
#define UI_GAP    6
#define UI_LV_W   332
#define UI_BTN_W  132
#define UI_ROWS   17   /* tall enough for the 8-button column + more packages */

static struct Gadget *build_gadgets(void)
{
    struct Gadget *gad, *glist = NULL;
    struct NewGadget ng;
    struct TextAttr *ta = g_scr->Font;
    WORD fh   = g_scr->RastPort.TxHeight;
    WORD btnH = fh + 7;
    WORD rowH = fh + 2;
    /* Gadget coords are relative to the window's OUTER top-left, so offset the
     * whole layout below the title bar (WBorTop + font) and inside the left
     * border — otherwise the header sits under the title bar ("too high"). */
    WORD wtop = g_scr->WBorTop + fh + 1;
    WORD wleft = g_scr->WBorLeft;
    WORD findH = fh + 6;
    WORD findY = wtop + UI_MARGIN;              /* header row 1: Find + View */
    WORD row2Y = findY + findH + UI_GAP;        /* header row 2: Category + Sort */
    WORD lvTop = row2Y + findH + UI_GAP;
    WORD lvH  = UI_ROWS * rowH;
    WORD statusH = fh + 6;
    WORD statusY = lvTop + lvH + UI_GAP;
    WORD progY = statusY + statusH + UI_GAP;    /* progress line below the status */
    WORD bx = wleft + UI_MARGIN + UI_LV_W + UI_GAP * 2;

    memset(&g_gads, 0, sizeof g_gads);
    gad = CreateContext(&glist);

    /* View cycle (Installed / Available) — top-RIGHT, directly above the button
     * column (its "Installed"/"Available" text is self-describing, no label). */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = bx; ng.ng_TopEdge = findY;
    ng.ng_Width = UI_BTN_W; ng.ng_Height = findH;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_VIEW;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)g_viewlabels, GTCY_Active, g_view, TAG_END);
    g_gads[GID_VIEW] = gad;

    /* Find box — filters the list by a case-insensitive id substring. */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN + 44; ng.ng_TopEdge = findY;
    ng.ng_Width = UI_LV_W - 44; ng.ng_Height = findH;
    ng.ng_GadgetText = (UBYTE *)"Find"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_SEARCH;
    gad = CreateGadget(STRING_KIND, gad, &ng,
                       GTST_String, (ULONG)g_filter, GTST_MaxChars, (ULONG)(sizeof g_filter - 1),
                       TAG_END);
    g_gads[GID_SEARCH] = gad;

    /* Header row 2: Category filter (left) + Sort mode (right half of list). */
    {
        size_t ci;
        g_catlabels[0] = "All categories";
        for (ci = 0; ci < g_ncats; ci++) g_catlabels[ci + 1] = g_catnames[ci];
        g_catlabels[g_ncats + 1] = NULL;
    }
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN; ng.ng_TopEdge = row2Y;
    ng.ng_Width = (UI_LV_W - UI_GAP) / 2; ng.ng_Height = findH;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_CAT;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)g_catlabels, GTCY_Active, g_cat_sel, TAG_END);
    g_gads[GID_CAT] = gad;

    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN + (UI_LV_W - UI_GAP) / 2 + UI_GAP; ng.ng_TopEdge = row2Y;
    ng.ng_Width = (UI_LV_W - UI_GAP) / 2; ng.ng_Height = findH;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_SORT;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)g_sortlabels, GTCY_Active, g_sort_recent, TAG_END);
    g_gads[GID_SORT] = gad;

    /* Listview of packages — rendered in the SYSTEM DEFAULT font, which is
     * guaranteed fixed-width, so the space-aligned columns line up (the screen
     * font is often proportional and made them ragged). */
    {
        static struct TextAttr fixedta;
        struct TextFont *df = GfxBase->DefaultFont;
        fixedta.ta_Name  = (STRPTR)df->tf_Message.mn_Node.ln_Name;
        fixedta.ta_YSize = df->tf_YSize;
        fixedta.ta_Style = FS_NORMAL;
        fixedta.ta_Flags = 0;
        memset(&ng, 0, sizeof ng);
        ng.ng_LeftEdge = wleft + UI_MARGIN; ng.ng_TopEdge = lvTop;
        ng.ng_Width = UI_LV_W; ng.ng_Height = lvH;
        ng.ng_TextAttr = &fixedta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_LIST; ng.ng_Flags = 0;
    }
    gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
                       GTLV_Labels, (ULONG)&g_list, GTLV_ShowSelected, 0L, TAG_END);
    g_gads[GID_LIST] = gad;

    /* Right-hand button column. */
    {
        WORD by = lvTop;
        struct { UBYTE *t; int id; int disabled; } btns[] = {
            {(UBYTE *)"Update Catalog", GID_UPDATE, 0},
            {(UBYTE *)"Check Updates", GID_CHECK,   0},
            {(UBYTE *)"Update All",    GID_UPGRADE, 0},
            {(UBYTE *)"Info",          GID_INFO,    1},
            {(UBYTE *)"Install",       GID_INSTALL, 1},
            {(UBYTE *)"Run",           GID_RUN,     1},
            {(UBYTE *)"Remove",        GID_REMOVE,  1},
            {(UBYTE *)"Refresh",       GID_REFRESH, 0},
        };
        int i;
        for (i = 0; i < 8; i++) {
            memset(&ng, 0, sizeof ng);
            ng.ng_LeftEdge = bx; ng.ng_TopEdge = by;
            ng.ng_Width = UI_BTN_W; ng.ng_Height = btnH;
            ng.ng_GadgetText = btns[i].t;
            ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = btns[i].id; ng.ng_Flags = 0;
            gad = CreateGadget(BUTTON_KIND, gad, &ng,
                               GA_Disabled, btns[i].disabled ? TRUE : FALSE, TAG_END);
            g_gads[btns[i].id] = gad;
            by += btnH + UI_GAP;
        }
    }

    /* Status line across the foot. */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN; ng.ng_TopEdge = statusY;
    ng.ng_Width = UI_LV_W + UI_GAP * 2 + UI_BTN_W; ng.ng_Height = statusH;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_STATUS; ng.ng_Flags = 0;
    status_count();
    gad = CreateGadget(TEXT_KIND, gad, &ng,
                       GTTX_Text, (ULONG)g_statusbuf, GTTX_Border, TRUE, GTTX_Clipped, TRUE, TAG_END);
    g_gads[GID_STATUS] = gad;

    /* Progress line — the running operation's last output line. */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN; ng.ng_TopEdge = progY;
    ng.ng_Width = UI_LV_W + UI_GAP * 2 + UI_BTN_W; ng.ng_Height = statusH;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_PROGRESS; ng.ng_Flags = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
                       GTTX_Text, (ULONG)g_progressbuf, GTTX_Border, TRUE, GTTX_Clipped, TRUE, TAG_END);
    g_gads[GID_PROGRESS] = gad;

    g_glist = glist;
    return glist;
}

static WORD g_winx = 40, g_winy = 22;   /* window position, persisted */

static int open_ui(void)
{
    WORD fh, w, h;
    if (!(g_scr = LockPubScreen(NULL))) return 0;
    if (!(g_vi = GetVisualInfo(g_scr, TAG_END))) return 0;
    fh = g_scr->RastPort.TxHeight;
    rebuild_list();
    if (!build_gadgets()) return 0;

    w = UI_MARGIN + UI_LV_W + UI_GAP * 2 + UI_BTN_W + UI_MARGIN;
    /* margin + header rows (find/view + category/sort) + gap + list + gap
     * + status + gap + progress + margin */
    h = UI_MARGIN + (fh + 6) + UI_GAP + (fh + 6) + UI_GAP
        + (UI_ROWS * (fh + 2)) + UI_GAP + (fh + 6) + UI_GAP + (fh + 6) + UI_MARGIN;
    {   /* Restore the last window position (saved on close). */
        char *pos = read_file(AMIPKG_CONFIG_DIR "winpos");
        if (pos) {
            long x = atol(pos), y = 0;
            char *sp = strchr(pos, ' ');
            if (sp) y = atol(sp + 1);
            if (x >= 0 && x < 2048 && y >= 0 && y < 2048) { g_winx = (WORD)x; g_winy = (WORD)y; }
            free(pos);
        }
    }
    g_win = OpenWindowTags(NULL,
        WA_Title,        (ULONG)"amipkg - AmigaImager Package Manager",
        WA_Left, g_winx, WA_Top, g_winy, WA_InnerWidth, w, WA_InnerHeight, h,
        WA_Flags,        WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET
                       | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        /* Render menus with the screen's NewLook pens (white menu, black
         * text on a standard WB scheme) — without this Intuition falls back
         * to the 1.x-style pens: black menu, grey text (tester report). */
        WA_NewLookMenus, TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK
                       | IDCMP_INTUITICKS
                       | CYCLEIDCMP | LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP,
        WA_Gadgets,      (ULONG)g_glist,
        WA_PubScreen,    (ULONG)g_scr,
        TAG_END);
    if (!g_win) return 0;
    GT_RefreshWindow(g_win, NULL);

    if ((g_menu = CreateMenus(g_newmenu, TAG_END))) {
        if (LayoutMenus(g_menu, g_vi, GTMN_TextAttr, (ULONG)g_scr->Font,
                        GTMN_NewLookMenus, TRUE, TAG_END))
            SetMenuStrip(g_win, g_menu);
        else { FreeMenus(g_menu); g_menu = NULL; }
    }
    update_action_state();
    return 1;
}

static void close_ui(void)
{
    if (g_win) {
        /* Remember the window position for the next run. */
        FILE *f = fopen(AMIPKG_CONFIG_DIR "winpos", "w");
        if (f) { fprintf(f, "%d %d\n", g_win->LeftEdge, g_win->TopEdge); fclose(f); }
    }
    if (g_win && g_menu) ClearMenuStrip(g_win);
    if (g_win) { CloseWindow(g_win); g_win = NULL; }
    if (g_menu) { FreeMenus(g_menu); g_menu = NULL; }
    if (g_glist) { FreeGadgets(g_glist); g_glist = NULL; }
    if (g_vi) { FreeVisualInfo(g_vi); g_vi = NULL; }
    if (g_scr) { UnlockPubScreen(NULL, g_scr); g_scr = NULL; }
}

static int dispatch_menu(UWORD menuNum, UWORD itemNum)
{
    if (menuNum == MENU_PROJECT) {
        if (itemNum == PROJ_ABOUT) action_about();
        else if (itemNum == PROJ_DOCS) {
            SystemTags((STRPTR)"Run >NIL: SYS:Utilities/MultiView AMIPKG:amipkg.guide", TAG_DONE);
            set_status("Opening the documentation (MultiView)...");
        }
        else if (itemNum == PROJ_QUIT) return 1;
    } else if (menuNum == MENU_PACKAGE) {
        if (itemNum == PKG_UPDATECAT) action_update_catalog();
        else if (itemNum == PKG_CHECK) action_check();
        else if (itemNum == PKG_UPGRADE) action_upgrade();
        else if (itemNum == PKG_INFO) action_info();
        else if (itemNum == PKG_INSTALL) action_install();
        else if (itemNum == PKG_ADOPT) action_adopt();
        else if (itemNum == PKG_REMOVE) action_remove();
        else if (itemNum == PKG_REFRESH) action_refresh();
        else if (itemNum == PKG_SETDIR) action_set_dir();
    }
    return 0;
}

static void event_loop(void)
{
    int done = 0;
    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(g_win->UserPort);
        while ((imsg = GT_GetIMsg(g_win->UserPort))) {
            ULONG cls = imsg->Class;
            UWORD code = imsg->Code;
            ULONG secs = imsg->Seconds, mics = imsg->Micros;
            struct Gadget *g = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                done = 1;
                break;
            case IDCMP_INTUITICKS:
                poll_async();
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(g_win); GT_EndRefresh(g_win, TRUE);
                break;
            case IDCMP_MENUPICK: {
                UWORD mn = code;
                while (mn != MENUNULL && !done) {
                    struct MenuItem *it = ItemAddress(g_menu, mn);
                    if (dispatch_menu(MENUNUM(mn), ITEMNUM(mn))) done = 1;
                    mn = it ? it->NextSelect : MENUNULL;
                }
                break;
            }
            case IDCMP_GADGETUP:
                switch (g->GadgetID) {
                case GID_VIEW: switch_view((int)code); break;
                case GID_SEARCH: action_search(g); break;
                case GID_CAT:  g_cat_sel = (int)code;     action_refresh(); break;
                case GID_SORT: g_sort_recent = (int)code; action_refresh(); break;
                case GID_RUN:  action_run(); break;
                case GID_LIST: {
                    static ULONG lastSec = 0, lastMic = 0;
                    static int lastRow = -1;
                    int dbl = (int)code == lastRow
                              && DoubleClick(lastSec, lastMic, secs, mics);
                    lastSec = secs; lastMic = mics; lastRow = (int)code;
                    g_selected = (int)code;
                    update_action_state();
                    if (g_selected >= 0 && (size_t)g_selected < g_nrows) {
                        char st[128];
                        if (g_rowdesc[g_selected][0])
                            snprintf(st, sizeof st, "%s - %s",
                                     g_rowid[g_selected], g_rowdesc[g_selected]);
                        else
                            snprintf(st, sizeof st, "Selected: %s", g_rowid[g_selected]);
                        set_status(st);
                    }
                    if (dbl) action_info();
                    break;
                }
                case GID_UPDATE:  action_update_catalog(); break;
                case GID_CHECK:   action_check();   break;
                case GID_UPGRADE: action_upgrade(); break;
                case GID_INFO:    action_info();    break;
                case GID_INSTALL: action_install(); break;
                case GID_REMOVE:  action_remove();  break;
                case GID_REFRESH: action_refresh(); break;
                }
                break;
            }
        }
    }
}

static int gui_run(void)
{
    int rc = 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GadToolsBase  = OpenLibrary("gadtools.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    AslBase       = OpenLibrary("asl.library", 37);   /* optional (drawer picker) */
    if (!IntuitionBase || !GadToolsBase || !GfxBase) {
        if (AslBase)       CloseLibrary(AslBase);
        if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
        if (GadToolsBase)  CloseLibrary(GadToolsBase);
        if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
        return 20;
    }
    amipkg_bridge_assigns();   /* resolve/bootstrap AMIPKG: (home drawer) */
    if (open_ui()) { event_loop(); rc = 0; }
    close_ui();
    if (AslBase) CloseLibrary(AslBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/* Run on a guaranteed 128 KB stack (Workbench/Shell may launch us with as
 * little as 4 KB): the index parse recurses and GadTools needs headroom. Same
 * StackSwap idiom as the CLI — globals carry the result across the swap. */
#include <exec/tasks.h>

static struct StackSwapStruct g_sss;
static char *g_stk;
static int   g_rc;

int main(void)
{
    g_stk = (char *)AllocMem(128UL * 1024UL, MEMF_ANY);
    if (!g_stk) return gui_run();
    g_sss.stk_Lower   = (APTR)g_stk;
    g_sss.stk_Upper   = (ULONG)g_stk + 128UL * 1024UL;
    g_sss.stk_Pointer = (APTR)((ULONG)g_stk + 128UL * 1024UL);
    StackSwap(&g_sss);
    g_rc = gui_run();
    StackSwap(&g_sss);
    FreeMem(g_stk, 128UL * 1024UL);
    return g_rc;
}

#else
int main(void) { return 0; }   /* host build: GUI is Amiga-only */
#endif
