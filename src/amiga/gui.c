/*
 * gui.c - amipkg-gui, a GadTools front-end for the AmigaImager package manager.
 *
 * A Workbench GUI for technical users over the same on-image data the CLI reads
 * (the receipt DB + seeded index via src/core/store.c). Reads are structured
 * (the listview is populated from load_installed / the parsed index); the
 * mutating actions (install / remove) shell out to the amipkg CLI so the validated
 * CLI logic is the single source of truth and cannot drift.
 *
 * Toolkit: GadTools (gadtools.library over Intuition) - core OS since 2.0, so
 * this runs on every AmigaImager-built image (3.0-3.9) with no MUI/ReAction
 * dependency. v37+ (Kickstart 2.04) required, which every target exceeds.
 *
 * A "View" cycle switches the listview between INSTALLED packages (from the
 * receipt DB) and AVAILABLE packages (the whole seeded index - what the repo
 * offers). Buttons act on the selection: Info always; Install in the Available
 * view; Remove in the Installed view. Results land in a status line at the foot
 * of the window (requesters only for detail or confirmation).
 *
 * PARITY RULE: gui.c and mui.c (the MUI front-end) are maintained in
 * LOCKSTEP - every user-facing feature lands in BOTH in the same change.
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
#include "../core/arepo.h"

/* The catalog the GUI shows: EVERY ENABLED REPO merged, in priority order -
 * exactly what the CLI resolves against. The two front-ends must never
 * disagree about what is available. Returns 1 on success (free with
 * aidx_free), 0 when no repo has a catalog yet. */
static int gui_load_index(aidx_index *idx) { return arepo_load_merged(idx) == 0; }

#include "../core/aindex.h"
#include "../core/aver.h"

#define AMIPKG_GUI_VERSION "amipkg-gui " AMIPKG_VERSION

/* AmigaOS Version-command tag: `Version SYS:Tools/amipkg-gui` reports the
 * exact build - essential for tester feedback. `used` keeps -Os from
 * discarding the unreferenced constant. */
static const char verstag[] __attribute__((used)) = "$VER: amipkg-gui " AMIPKG_VERSION " (" AMIPKG_VERDATE ")";

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
       GID_CAT, GID_SORT, GID_ADOPT, GID_UPDATE1 };

/* Menu numbering (must match g_newmenu below). */
/* Menu/item numbers are POSITIONAL - they must track g_newmenu exactly. */
enum { MENU_APP = 0, MENU_PACKAGE = 1, MENU_SETTINGS = 2 };
enum { APP_ABOUT = 0, APP_DOCS = 1, APP_QUIT = 3 };     /* item 2 is the bar */
enum { PKG_UPDATECAT = 0, PKG_CHECK = 1, PKG_UPGRADE = 2, PKG_INFO = 3, PKG_INSTALL = 4,
       PKG_INSTALLTO = 5, PKG_UPDATE1 = 6, PKG_ADOPT = 7, PKG_SUBMIT = 8,
       PKG_REMOVE = 9, PKG_REFRESH = 10 };
enum { SET_DIR = 0, SET_REPOS = 1 };

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
static struct Gadget *g_gads[GID_ADOPT + 1];
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
static char g_busy_verb[48];      /* "Install of 'foo'" - for the finish message */
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
    /* "Project" is for apps that load/save project files; amipkg has none, so
     * the menu carries the app name (MorphOS/MUI Style Guide). Kept identical
     * to amipkg-mui's menustrip - the two front-ends must not drift. */
    { NM_TITLE, (STRPTR)"amipkg",        NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"About...",      (STRPTR)"?", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Documentation...", NULL,     0, 0, NULL },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,     NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Quit",          (STRPTR)"Q", 0, 0, NULL },
    { NM_TITLE, (STRPTR)"Package",       NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Update Catalog", (STRPTR)"A", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Check Updates", (STRPTR)"C", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Update All",    (STRPTR)"U", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Info",          (STRPTR)"I", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Install",       (STRPTR)"N", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Install To...",  (STRPTR)"T", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Update",         (STRPTR)"G", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Adopt Existing...", NULL,    0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Submit a Package...", NULL,  0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Remove",        (STRPTR)"R", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Refresh",       (STRPTR)"F", 0, 0, NULL },
    { NM_TITLE, (STRPTR)"Settings",      NULL,        0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Install Drawer...", (STRPTR)"D", 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Repositories...", (STRPTR)"E", 0, 0, NULL },
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

/* Launch the AmigaGuide manual. MultiView is a SEPARATE process, so
 * "PROGDIR:" would resolve to MULTIVIEW's drawer - build the absolute
 * path of OUR drawer when the home is PROGDIR:-based (no assign). */
static void open_docs(void)
{
    char cmd[400], dir[256];
    if (strcmp(amipkg_prefix(), "PROGDIR:") == 0) {
        BPTR pd = GetProgramDir();
        dir[0] = 0;
        if (!pd || !NameFromLock(pd, (STRPTR)dir, sizeof dir)) dir[0] = 0;
        if (dir[0]) {
            snprintf(cmd, sizeof cmd,
                     "Run >NIL: SYS:Utilities/MultiView \"%s%samipkg.guide\"",
                     dir, dir[strlen(dir) - 1] == ':' ? "" : "/");
        } else
            snprintf(cmd, sizeof cmd, "Run >NIL: SYS:Utilities/MultiView PROGDIR:amipkg.guide");
    } else
        snprintf(cmd, sizeof cmd, "Run >NIL: SYS:Utilities/MultiView %samipkg.guide", amipkg_prefix());
    SystemTags((STRPTR)cmd, TAG_DONE);
}
static const char *cli_path(void)
{
    static char path[300] = "";
    if (!path[0]) {
        /* OWN DRAWER FIRST: the CLI next to this GUI is by definition the
         * matching version. CRITICAL: as an ABSOLUTE path, quoted - the
         * async ops run the command through an Execute'd shell, and that
         * shell process has NO PROGDIR: of its own ("please insert volume
         * PROGDIR:" requester, tester report 0.4.6). */
        BPTR l = Lock((STRPTR)"PROGDIR:amipkg", ACCESS_READ);
        if (l) {
            char dir[256];
            BPTR pd = GetProgramDir();
            UnLock(l);
            dir[0] = 0;
            if (pd && NameFromLock(pd, (STRPTR)dir, sizeof dir) && dir[0]) {
                snprintf(path, sizeof path, "\"%s%samipkg\"",
                         dir, dir[strlen(dir) - 1] == ':' ? "" : "/");
                return path;
            }
        }
        l = Lock((STRPTR)"AMIPKG:amipkg", ACCESS_READ);
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
static void req(const char *title, const char *body);   /* defined below */

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
/* Two chained CLI commands in ONE async script (e.g. "update" then
 * "upgrade" - Update All must never act on a stale catalog). Output of both
 * lands in ASYNC_OUT; the recorded rc is the SECOND command's. FailAt 30
 * lets the chain continue past a failed update (rc 10) - the second command
 * then behaves exactly like the old single-command path. */
static int run_async2(const char *cmd1, const char *cmd2, const char *verb);

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

static int run_async2(const char *cmd1, const char *cmd2, const char *verb)
{
    BPTR fh, in, out;
    static char script[1024];
    if (g_busy) { set_status("An operation is already running."); return 0; }
    DeleteFile((STRPTR)ASYNC_OUT);
    DeleteFile((STRPTR)ASYNC_DONE);
    snprintf(script, sizeof script,
             "FailAt 30\n%s >" ASYNC_OUT "\n%s >>" ASYNC_OUT "\nEcho $RC >" ASYNC_DONE "\n",
             cmd1, cmd2);
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


/* Friendlier progress: the http layer's raw download lines
 * ("  123/456K (27%)") become "Downloading: 27% (123 of 456 KB)";
 * everything else is shown as-is. */
static void show_progress_line(const char *line)
{
    long got, total, pct;
    if (sscanf(line, " %ld/%ldK (%ld%%)", &got, &total, &pct) == 3) {
        char b[96];
        snprintf(b, sizeof b, "Downloading: %ld%%  (%ld of %ld KB)", pct, got, total);
        set_progress(b);
    } else set_progress(line);
}
/* One INTUITICKS heartbeat while an async op runs. */
/* Did the finished operation update amipkg ITSELF? The CLI prints a marker
 * NOTE line; scan the output for it (streamed with overlap - the file can be
 * a long Update All log). */
static int output_mentions_selfupdate(void)
{
    static char buf[4096];
    const char *needle = "amipkg itself was updated";
    size_t nl = strlen(needle), keep = 0;
    int found = 0;
    BPTR fh = Open((STRPTR)ASYNC_OUT, MODE_OLDFILE);
    LONG n;
    if (!fh) return 0;
    while (!found && (n = Read(fh, buf + keep, (LONG)(sizeof buf - keep - 1))) > 0) {
        size_t total = keep + (size_t)n;
        buf[total] = 0;
        if (strstr(buf, needle)) { found = 1; break; }
        keep = total > nl ? nl : total;
        memmove(buf, buf + total - keep, keep);
    }
    Close(fh);
    return found;
}

static void poll_async(void)
{
    char line[160];
    BPTR fh;
    if (!g_busy) return;
    g_busy_ticks++;
    /* Living status: a classic spinner plus elapsed m:ss (parity with
     * amipkg-mui) - updated every 2nd tick (~5 Hz) to keep GT_SetGadgetAttrs
     * traffic modest. */
    if ((g_busy_ticks % 2) == 0) {
        static const char spin[] = "|/-\\";
        char b[96];
        long secs = g_busy_ticks / 10;
        snprintf(b, sizeof b, "%s %c  %ld:%02ld", g_busy_verb,
                 spin[(g_busy_ticks / 2) & 3], secs / 60, secs % 60);
        set_status(b);
    }
    /* Stream progress ~2x/s (ticks fire ~10x/s). */
    if ((g_busy_ticks % 5) == 0) {
        tail_line(ASYNC_OUT, line, sizeof line);
        if (line[0]) show_progress_line(line);
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
            if (rc == 0 && output_mentions_selfupdate())
                req("amipkg updated",
                    "amipkg itself was updated.\n\n"
                    "This window is still running the PREVIOUS version -\n"
                    "please close and restart amipkg to use the new one.");
            /* Status lines truncate - a failure ALWAYS pops the full CLI
             * message in a requester (parity with amipkg-mui). */
            if (rc != 0) {
                static char body[280];
                snprintf(body, sizeof body, "%s failed (rc %ld).\n\n%s",
                         g_busy_verb, rc,
                         line[0] ? line : "(no output - check RAM:amipkg-gui.out)");
                req("amipkg - operation failed", body);
            }
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
static int selection_has_update(void);  /* defined below */
static int pkg_has_update(const rcpt_installed *inst, size_t ninst,
                          const aidx_entry *e);   /* defined below */

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
    if (g_gads[GID_ADOPT])
        GT_SetGadgetAttrs(g_gads[GID_ADOPT], g_win, NULL,
                          GA_Disabled, (!busy && sel) ? FALSE : TRUE, TAG_END);
    if (g_gads[GID_UPDATE1])
        GT_SetGadgetAttrs(g_gads[GID_UPDATE1], g_win, NULL,
                          GA_Disabled, (!busy && sel && selection_has_update()) ? FALSE : TRUE, TAG_END);
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
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t ninst, i;
    char label[80];
    init_list(&g_list);
    g_nrows = 0;
    ninst = load_installed(inst, MAX_PKGS);

    if (g_view == VIEW_INSTALLED) {
        /* Old build-time receipts have no version ("-"): fall back to the
         * catalog's (build-time installs came from that same catalog). */
        aidx_index vidx;
        int have_vidx = gui_load_index(&vidx);
        for (i = 0; i < ninst; i++) {
            const char *v;
            char vguess[56];
            if (!contains_ci(inst[i].id, g_filter)) continue;
            v = inst[i].version;
            if (have_vidx && aver_is_unknown(v)) {
                /* No version in the receipt. The catalog's is a GUESS, not a
                 * fact - mark it "?" so it does not read as what is really
                 * installed. (djbase, 2026-07-29: an adopted package showed
                 * the repo's version, then lost it when the repo was
                 * disabled - because this substitution was silent.) */
                const aidx_entry *e = aidx_find(&vidx, inst[i].id);
                if (e && !aver_is_unknown(e->version)) {
                    snprintf(vguess, sizeof vguess, "%s?", e->version);
                    v = vguess;
                }
            }
            {   /* flag the ones worth acting on, same test as the button */
                const aidx_entry *ce = have_vidx ? aidx_find(&vidx, inst[i].id) : NULL;
                snprintf(label, sizeof label, "%-22s %-10s %s",
                         inst[i].id, shown_version(v),
                         pkg_has_update(inst, ninst, ce) ? "UPDATE" : "");
            }
            add_row(label, inst[i].id, NULL);
        }
        if (have_vidx) aidx_free(&vidx);
        /* First run opens in this view: still harvest the category labels so
         * the filter cycle is populated before the first switch to Available. */
        if (g_ncats == 0) {
            aidx_index idx;
            if (gui_load_index(&idx)) {
                for (i = 0; i < idx.count; i++) note_category(idx.entries[i].category);
                aidx_free(&idx);
            }
        }
    } else {
        aidx_index idx;
        if (gui_load_index(&idx)) {
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
                /* Hide what this machine cannot run (djbase's Architecture
                 * request). MorphOS/OS4 still see the 68k catalog - see
                 * aidx_arch_runs_on - so this only removes the genuinely
                 * unusable. */
                if (!aidx_arch_runs_on(aidx_arch(e), amipkg_host_arch())) continue;
                /* Leading marker so installed state is scannable at a glance. */
                snprintf(label, sizeof label, "%s %-18s %-8s %s",
                         id_installed(inst, ninst, e->id) ? "*" : " ",
                         e->id, shown_version(e->version),
                         pkg_has_update(inst, ninst, e) ? "UPDATE"
                             : (id_installed(inst, ninst, e->id) ? "installed" : ""));
                add_row(label, e->id, e->description);
            }
            aidx_free(&idx);
        } else {
            set_status("No catalog yet - click Update Catalog.");
        }
    }
}

/* Is a NEWER version available for `id` than what the receipt records?
 *
 * "Update All" has always existed, but there was no way to update ONE package
 * from the GUI even though the CLI has taken `upgrade <id>` all along. This is
 * what makes the per-package button meaningful: it lights up only when there is
 * something to do, and the same test marks the row. An unknown installed
 * version ("-") is never "outdated" - aver_is_newer already refuses those. */
static int pkg_has_update(const rcpt_installed *inst, size_t ninst,
                          const aidx_entry *e)
{
    size_t k;
    if (!e) return 0;
    for (k = 0; k < ninst; k++) {
        if (strcmp(inst[k].id, e->id) != 0) continue;
        return aver_is_newer(e->version, inst[k].version);
    }
    return 0;                                  /* not installed */
}

/* Does the SELECTED row have an update available? */
static int selection_has_update(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;
    size_t ninst;
    aidx_index idx;
    int yes = 0;
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) return 0;
    ninst = load_installed(inst, MAX_PKGS);
    if (!gui_load_index(&idx)) return 0;
    yes = pkg_has_update(inst, ninst, aidx_find(&idx, g_rowid[g_selected]));
    aidx_free(&idx);
    return yes;
}

/* Is the currently-selected row an installed package (receipt DB)? */
static int selection_installed(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
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

/* "Update Catalog" - fetch the latest signed index from the repo, Ed25519-verify
 * it on-device, replace the local catalog, and reload the list. Shells to
 * `C:amipkg update` (needs a TCP/IP stack up). */
static void action_update_catalog(void)
{
    set_status("Updating catalog...");
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s update", cli_path());
    run_async(cmd, "Catalog update");
}

static void action_check(void)
{
    static aidx_index idx;
    char msg[2048];
    size_t used = 0, i;
    int updates = 0;
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t ninst;
    set_status("Checking for updates...");
    if (!gui_load_index(&idx)) { set_status("No catalog yet - click Update Catalog."); return; }
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

/* "Update All" - upgrade every out-of-date package via `C:amipkg upgrade`.
 * Each upgrade removes the old version's files first, then installs the new. */
static void action_upgrade(void)
{
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!req_confirm("Update All",
                     "Upgrade every out-of-date package?\n\nDownloads + reinstalls the newer\n"
                     "versions.", "Update|Cancel")) {
        set_status("Update cancelled."); return;
    }
    set_status("Updating all packages...");
    {
        /* Refresh the catalog FIRST - "Update All" must never act on a
         * stale index (A4000 report: upgrade said up-to-date against an
         * old local catalog). */
        char c1[340], c2[340];
        snprintf(c1, sizeof c1, "%s update", cli_path());
        snprintf(c2, sizeof c2, "%s upgrade", cli_path());
        run_async2(c1, c2, "Update All");
    }
}

static void action_info(void)
{
    aidx_index idx;
    const aidx_entry *e;
    char msg[1024];
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t ninst, k;
    const char *id, *insver = "(not installed)";
    int ins;
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    id = g_rowid[g_selected];
    if (!gui_load_index(&idx)) { set_status("No catalog yet - click Update Catalog."); return; }
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
        if (e->arch[0] && strcmp(aidx_arch(e), "m68k-amigaos") != 0)
            u += (size_t)snprintf(msg + u, sizeof msg - u, "architecture: %s\n", aidx_arch(e));
        {   /* Which repository this came from. Quiet in the single-repo
             * default, where it would only be noise. */
            arepo_list rl; arepo_load(&rl);
            if (rl.count > 1) {
                const char *rp = e->repo[0] ? e->repo : AREPO_OFFICIAL_ID;
                int at = arepo_find(&rl, rp);
                u += (size_t)snprintf(msg + u, sizeof msg - u, "repo: %s%s\n", rp,
                        (at >= 0 && !arepo_is_signed(&rl.v[at])) ? " (UNSIGNED)" : "");
            }
        }
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

/* "Adopt Existing..." - the selected catalog package is ALREADY on this
 * system somewhere: pick its drawer, and amipkg takes over managing it
 * (inventory + receipt + in-place upgrades). jdb78's idea. */
/* Modal "Submit a Package" form: a small GadTools window with id / URL /
 * description strings. On Submit it shells to `amipkg submit ...` like every
 * other mutation. Modal on purpose - one tiny event loop, no extra state. */
static void submit_form(void)
{
    static const char *cats[] = { "Utilities", "Games", "Internet", "Audio",
        "Text", "Network", "Graphics", "Development", "Libraries", "Emulation",
        "System", NULL };
    struct Window *w;
    struct Gadget *glist = NULL, *gad, *g_id, *g_url, *g_desc, *g_cat;
    WORD cat_sel = 0;
    struct NewGadget ng;
    struct TextAttr *ta = g_scr->Font;
    WORD fh = g_scr->RastPort.TxHeight;
    WORD rowH = fh + 6, gap = 6, labelW = 11 * (fh > 8 ? 8 : 7), fieldW = 340;
    WORD top = (WORD)(g_scr->WBorTop + fh + 1) + gap;
    WORD y = top, width, height;
    int done = 0, submit = 0;
    enum { SG_ID = 1, SG_URL, SG_DESC, SG_CAT, SG_GO, SG_CANCEL };

    if (g_busy) { set_status("An operation is already running."); return; }

    gad = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi;

    ng.ng_LeftEdge = 8 + labelW; ng.ng_TopEdge = y;
    ng.ng_Width = fieldW; ng.ng_Height = rowH;
    ng.ng_GadgetText = (UBYTE *)"Package id:"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = SG_ID;
    gad = g_id = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, 32, TAG_END);
    y += rowH + gap;

    ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)"Archive URL:"; ng.ng_GadgetID = SG_URL;
    gad = g_url = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, 254, TAG_END);
    y += rowH + gap;

    ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)"Description:"; ng.ng_GadgetID = SG_DESC;
    gad = g_desc = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, 158, TAG_END);
    y += rowH + gap;

    ng.ng_TopEdge = y; ng.ng_Width = 180;
    ng.ng_GadgetText = (UBYTE *)"Category:"; ng.ng_GadgetID = SG_CAT;
    gad = g_cat = CreateGadget(CYCLE_KIND, gad, &ng,
                               GTCY_Labels, (ULONG)cats, GTCY_Active, 0, TAG_END);
    ng.ng_Width = fieldW;
    y += rowH + gap + gap;

    ng.ng_LeftEdge = 8 + labelW; ng.ng_TopEdge = y;
    ng.ng_Width = 160; ng.ng_Height = rowH;
    ng.ng_GadgetText = (UBYTE *)"Submit for Review"; ng.ng_Flags = 0; ng.ng_GadgetID = SG_GO;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = 8 + labelW + 160 + gap; ng.ng_Width = 100;
    ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = SG_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    y += rowH + gap;

    if (!gad) { FreeGadgets(glist); set_status("Could not build the submit form."); return; }
    width = 8 + labelW + fieldW + 8;
    height = y + 4;

    w = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Submit a Package for Review",
        WA_InnerWidth, width, WA_InnerHeight, height - top + gap,
        WA_Left, (g_win ? g_win->LeftEdge + 40 : 80),
        WA_Top,  (g_win ? g_win->TopEdge + 40 : 60),
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET
                | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP,
        WA_Gadgets, (ULONG)glist,
        WA_PubScreen, (ULONG)g_scr,
        TAG_END);
    if (!w) { FreeGadgets(glist); set_status("Could not open the submit window."); return; }
    GT_RefreshWindow(w, NULL);
    ActivateGadget(g_id, w, NULL);

    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(w->UserPort);
        while ((imsg = GT_GetIMsg(w->UserPort)) != NULL) {
            ULONG cls = imsg->Class;
            struct Gadget *hit = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            if (cls == IDCMP_CLOSEWINDOW) done = 1;
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
            else if (cls == IDCMP_GADGETUP && hit) {
                if (hit->GadgetID == SG_CANCEL) done = 1;
                else if (hit->GadgetID == SG_GO) { done = 1; submit = 1; }
                else if (hit->GadgetID == SG_CAT) {
                    ULONG a = 0;
                    GT_GetGadgetAttrs(g_cat, w, NULL, GTCY_Active, (ULONG)&a, TAG_END);
                    cat_sel = (WORD)a;
                }
            }
        }
    }
    if (submit) {
        const char *sid  = (const char *)((struct StringInfo *)g_id->SpecialInfo)->Buffer;
        const char *surl = (const char *)((struct StringInfo *)g_url->SpecialInfo)->Buffer;
        const char *sdsc = (const char *)((struct StringInfo *)g_desc->SpecialInfo)->Buffer;
        if (!sid[0] || !surl[0]) {
            req("Submit a Package", "Package id and archive URL are both required.");
        } else {
            static char cmd[700], verb[48];
            snprintf(cmd, sizeof cmd, "%s submit %s \"%s\" CAT=%s \"%s\"",
                     cli_path(), sid, surl, cats[cat_sel], sdsc);
            snprintf(verb, sizeof verb, "Submission of '%s'", sid);
            set_status("Submitting for review...");
            CloseWindow(w); FreeGadgets(glist);
            run_async(cmd, verb);
            return;
        }
    }
    CloseWindow(w);
    FreeGadgets(glist);
}

/* ---- repositories --------------------------------------------------------- */

/* Modal "Add a repository" form: id / URL / public key. Same template as
 * submit_form. No networking and no crypto here - adding a repo is a config
 * edit, so it runs in-process rather than shelling to C:amipkg. */
static int repo_add_form(arepo_list *l)
{
    struct Window *w;
    struct Gadget *glist = NULL, *gad, *g_id, *g_url, *g_key;
    struct NewGadget ng;
    struct TextAttr *ta = g_scr->Font;
    WORD fh = g_scr->RastPort.TxHeight;
    WORD rowH = fh + 6, gap = 6, labelW = 11 * (fh > 8 ? 8 : 7), fieldW = 340;
    WORD top = (WORD)(g_scr->WBorTop + fh + 1) + gap;
    WORD y = top, width, height;
    int done = 0, add = 0, added = 0;
    enum { RG_ID = 1, RG_URL, RG_KEY, RG_GO, RG_CANCEL };

    gad = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi;

    ng.ng_LeftEdge = 8 + labelW; ng.ng_TopEdge = y;
    ng.ng_Width = fieldW; ng.ng_Height = rowH;
    ng.ng_GadgetText = (UBYTE *)"Name:"; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetID = RG_ID;
    gad = g_id = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, AREPO_ID_MAX - 1, TAG_END);
    y += rowH + gap;

    ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)"URL:"; ng.ng_GadgetID = RG_URL;
    gad = g_url = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, AREPO_URL_MAX - 2, TAG_END);
    y += rowH + gap;

    ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)"Public key:"; ng.ng_GadgetID = RG_KEY;
    gad = g_key = CreateGadget(STRING_KIND, gad, &ng, GTST_MaxChars, AREPO_KEY_MAX - 2, TAG_END);
    y += rowH + gap + gap;

    ng.ng_LeftEdge = 8 + labelW; ng.ng_TopEdge = y;
    ng.ng_Width = 120; ng.ng_Height = rowH;
    ng.ng_GadgetText = (UBYTE *)"Add"; ng.ng_Flags = 0; ng.ng_GadgetID = RG_GO;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    ng.ng_LeftEdge = 8 + labelW + 120 + gap; ng.ng_Width = 100;
    ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = RG_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    y += rowH + gap;

    if (!gad) { FreeGadgets(glist); set_status("Could not build the form."); return 0; }
    width = 8 + labelW + fieldW + 8;
    height = y + 4;

    w = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Add a Repository",
        WA_InnerWidth, width, WA_InnerHeight, height - top + gap,
        WA_Left, (g_win ? g_win->LeftEdge + 50 : 80),
        WA_Top,  (g_win ? g_win->TopEdge + 50 : 60),
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET
                | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP,
        WA_Gadgets, (ULONG)glist,
        WA_PubScreen, (ULONG)g_scr,
        TAG_END);
    if (!w) { FreeGadgets(glist); set_status("Could not open the window."); return 0; }
    GT_RefreshWindow(w, NULL);
    ActivateGadget(g_id, w, NULL);

    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(w->UserPort);
        while ((imsg = GT_GetIMsg(w->UserPort)) != NULL) {
            ULONG cls = imsg->Class;
            struct Gadget *hit = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            if (cls == IDCMP_CLOSEWINDOW) done = 1;
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
            else if (cls == IDCMP_GADGETUP && hit) {
                if (hit->GadgetID == RG_CANCEL) done = 1;
                else if (hit->GadgetID == RG_GO)  { done = 1; add = 1; }
            }
        }
    }

    if (add) {
        const char *sid  = (const char *)((struct StringInfo *)g_id->SpecialInfo)->Buffer;
        const char *surl = (const char *)((struct StringInfo *)g_url->SpecialInfo)->Buffer;
        const char *skey = (const char *)((struct StringInfo *)g_key->SpecialInfo)->Buffer;
        static char id[AREPO_ID_MAX], url[AREPO_URL_MAX], key[AREPO_KEY_MAX];
        int rc;
        strncpy(id, sid, sizeof id - 1);    id[sizeof id - 1] = '\0';
        strncpy(url, surl, sizeof url - 1); url[sizeof url - 1] = '\0';
        strncpy(key, skey, sizeof key - 1); key[sizeof key - 1] = '\0';
        CloseWindow(w); FreeGadgets(glist);

        if (arepo_id_valid(id) != 0) {
            req("Add a Repository",
                "That name will not do.\n\n"
                "Use letters, digits, - and _ only (it becomes a\n"
                "drawer name under repos/).");
            return 0;
        }
        if (arepo_url_valid(url) != 0) {
            req("Add a Repository", "The URL must start with http:// or https://.");
            return 0;
        }
        if (arepo_find(l, id) >= 0) {
            req("Add a Repository", "You already have a repository with that name.");
            return 0;
        }
        /* The ONE place the unsigned decision gets made in this GUI. Say what
         * it actually costs - see confirm_unsigned() in the CLI, same wording. */
        if (!key[0]) {
            if (!req_confirm("Unsigned Repository",
                    "This repository has no public key, so its catalog\n"
                    "will NOT be verified.\n\n"
                    "That means trusting the person running it AND every\n"
                    "hop in between - your ISP, router, any proxy. amipkg\n"
                    "fetches over plain HTTP, and an unsigned catalog can\n"
                    "be altered on the way to you. The archive checksums\n"
                    "do not help: they live inside that same catalog.\n\n"
                    "Ask the repo owner for their public key if they have one.\n\n"
                    "Add it as an UNSIGNED repository anyway?",
                    "Add Unsigned|Cancel"))
                return 0;
        }
        rc = arepo_add(l, id, url, key[0] ? key : NULL);
        if (rc == 5) { req("Add a Repository", "That does not look like a base64 public key."); return 0; }
        if (rc == 1) { req("Add a Repository", "The repository list is full."); return 0; }
        if (rc != 0) { req("Add a Repository", "Could not add that repository."); return 0; }
        if (arepo_save(l) != 0) { req("Add a Repository", "Could not save the repository list."); return 0; }
        added = 1;
    } else {
        CloseWindow(w);
        FreeGadgets(glist);
    }
    return added;
}

/* Modal repository manager: the list, plus the config edits. Everything here
 * is local file state, so no C:amipkg round-trip is needed - only 'Update'
 * afterwards touches the network, and that is the existing async path. */
static void repo_manager(void)
{
    static arepo_list rl;
    static struct List rlist;
    static struct Node rnodes[AREPO_MAX];
    static char rlabels[AREPO_MAX][90];
    struct Window *w;
    struct Gadget *glist = NULL, *gad, *lv;
    struct NewGadget ng;
    struct TextAttr *ta = g_scr->Font;
    WORD fh = g_scr->RastPort.TxHeight;
    WORD rowH = fh + 6, gap = 6;
    WORD top = (WORD)(g_scr->WBorTop + fh + 1) + gap;
    WORD lvW = 380, lvH = rowH * 6, btnW = 110;
    WORD y = top, width, height;
    int done = 0, sel = 0, dirty = 0;
    size_t i;
    enum { RM_LIST = 1, RM_ADD, RM_REMOVE, RM_TOGGLE, RM_UP, RM_DOWN, RM_CLOSE };

    if (g_busy) { set_status("An operation is already running."); return; }
    arepo_load(&rl);

    gad = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi;

    ng.ng_LeftEdge = 8; ng.ng_TopEdge = y;
    ng.ng_Width = lvW; ng.ng_Height = lvH; ng.ng_GadgetID = RM_LIST; ng.ng_Flags = 0;
    ng.ng_GadgetText = NULL;
    init_list(&rlist);
    gad = lv = CreateGadget(LISTVIEW_KIND, gad, &ng,
                            GTLV_Labels, (ULONG)&rlist, GTLV_ShowSelected, 0L, TAG_END);

    {
        WORD by = top;
        struct { UBYTE *t; int id; } btns[] = {
            {(UBYTE *)"Add...",     RM_ADD},
            {(UBYTE *)"Remove",     RM_REMOVE},
            {(UBYTE *)"Enable/Off", RM_TOGGLE},
            {(UBYTE *)"Move Up",    RM_UP},
            {(UBYTE *)"Move Down",  RM_DOWN},
            {(UBYTE *)"Close",      RM_CLOSE},
        };
        size_t b;
        ng.ng_LeftEdge = 8 + lvW + gap; ng.ng_Width = btnW; ng.ng_Height = rowH;
        for (b = 0; b < sizeof btns / sizeof btns[0]; b++) {
            ng.ng_TopEdge = by; ng.ng_GadgetText = btns[b].t; ng.ng_GadgetID = btns[b].id;
            gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
            by += rowH + gap;
        }
        y = (by > top + lvH) ? by : (WORD)(top + lvH + gap);
    }

    if (!gad) { FreeGadgets(glist); set_status("Could not build the repository window."); return; }
    width = 8 + lvW + gap + btnW + 8;
    height = y + 4;

    w = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Repositories - order is priority",
        WA_InnerWidth, width, WA_InnerHeight, height - top + gap,
        WA_Left, (g_win ? g_win->LeftEdge + 30 : 60),
        WA_Top,  (g_win ? g_win->TopEdge + 30 : 40),
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET
                | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_Gadgets, (ULONG)glist,
        WA_PubScreen, (ULONG)g_scr,
        TAG_END);
    if (!w) { FreeGadgets(glist); set_status("Could not open the repository window."); return; }

    /* Rebuild the visible list from rl. Detach first: GadTools must not walk a
     * List while we are rewriting its nodes. */
#define REPO_RELIST() do {                                                      \
        GT_SetGadgetAttrs(lv, w, NULL, GTLV_Labels, ~0L, TAG_END);              \
        arepo_load(&rl);   /* re-read: every mutation saved first, and an   */  \
                           /* emptied list comes back as the official repo, */  \
                           /* so the window must show what is really there. */  \
        init_list(&rlist);                                                      \
        for (i = 0; i < rl.count; i++) {                                        \
            snprintf(rlabels[i], sizeof rlabels[i], "%d. %-14s %-8s %s",        \
                     (int)(i + 1), rl.v[i].id,                                  \
                     rl.v[i].enabled ? "on" : "OFF",                            \
                     arepo_is_signed(&rl.v[i]) ? "signed" : "UNSIGNED");        \
            rnodes[i].ln_Name = rlabels[i];                                     \
            AddTail(&rlist, &rnodes[i]);                                        \
        }                                                                       \
        GT_SetGadgetAttrs(lv, w, NULL, GTLV_Labels, (ULONG)&rlist, TAG_END);    \
    } while (0)

    REPO_RELIST();
    GT_RefreshWindow(w, NULL);

    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(w->UserPort);
        while ((imsg = GT_GetIMsg(w->UserPort)) != NULL) {
            ULONG cls = imsg->Class;
            UWORD code = imsg->Code;
            struct Gadget *hit = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            if (cls == IDCMP_CLOSEWINDOW) { done = 1; continue; }
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); continue; }
            if (cls != IDCMP_GADGETUP || !hit) continue;

            switch (hit->GadgetID) {
            case RM_LIST:
                sel = (int)code;
                break;
            case RM_CLOSE:
                done = 1;
                break;
            case RM_ADD:
                if (repo_add_form(&rl)) { dirty = 1; REPO_RELIST(); }
                break;
            case RM_REMOVE:
                if (sel < 0 || (size_t)sel >= rl.count) break;
                if (req_confirm("Remove Repository",
                                "Remove this repository from the list?\n\n"
                                "Packages already installed from it stay\n"
                                "installed; only the source goes away.",
                                "Remove|Cancel")) {
                    arepo_remove(&rl, rl.v[sel].id);
                    if (arepo_save(&rl) == 0) dirty = 1;
                    if (sel > 0 && (size_t)sel >= rl.count) sel--;
                    REPO_RELIST();
                }
                break;
            case RM_TOGGLE:
                if (sel < 0 || (size_t)sel >= rl.count) break;
                arepo_set_enabled(&rl, rl.v[sel].id, !rl.v[sel].enabled);
                if (arepo_save(&rl) == 0) dirty = 1;
                REPO_RELIST();
                break;
            case RM_UP:
            case RM_DOWN:
                if (sel < 0 || (size_t)sel >= rl.count) break;
                {
                    int delta = (hit->GadgetID == RM_UP) ? -1 : 1;
                    int to = sel + delta;
                    if (to < 0 || (size_t)to >= rl.count) break;
                    arepo_move(&rl, rl.v[sel].id, delta);
                    if (arepo_save(&rl) == 0) dirty = 1;
                    sel = to;
                    REPO_RELIST();
                    GT_SetGadgetAttrs(lv, w, NULL, GTLV_Selected, (ULONG)sel, TAG_END);
                }
                break;
            default:
                break;
            }
        }
    }
#undef REPO_RELIST

    GT_SetGadgetAttrs(lv, w, NULL, GTLV_Labels, ~0L, TAG_END);
    CloseWindow(w);
    FreeGadgets(glist);

    if (dirty) {
        /* The merged catalog just changed shape - repopulate, and point at the
         * one thing that actually fetches anything. */
        action_refresh_after_op();
        set_status("Repositories changed - run Update Catalog to fetch them.");
    }
}

static void action_adopt(void)
{
    struct FileRequester *fr;
    char cmd[400], verb[48];
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) {
        /* A status line is too easy to miss - explain the flow in a
         * requester instead (parity with amipkg-mui). */
        req("Adopt Existing",
            "Adopt puts an app you ALREADY have under amipkg's care:\n\n"
            "1. Select the matching package in the list first\n"
            "   (any view - e.g. 'visage' for your Visage install).\n"
            "2. Package menu -> Adopt Existing...\n"
            "3. Pick the drawer where the app lives.\n\n"
            "amipkg then inventories that drawer: updates land there,\n"
            "and Remove can cleanly uninstall it.");
        return;
    }
    if (!AslBase) { set_status("asl.library unavailable - use: amipkg adopt <id> <drawer>"); return; }
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,   (ULONG)"Where is it installed? (pick its drawer)",
            ASLFR_DrawersOnly, TRUE,
            g_win ? ASLFR_Window : TAG_IGNORE, (ULONG)g_win,
            ASLFR_SleepWindow, TRUE,
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

/* "Install Drawer..." - pick where recipe-less packages install to, via the ASL
 * drawer requester, and persist it (shared with the CLI's `amipkg dir`). */
static void action_set_dir(void)
{
    struct FileRequester *fr;
    char cur[256], b[300];
    if (!AslBase) { set_status("asl.library unavailable - use: amipkg dir <path>"); return; }
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

/* Install the selected package into a drawer the user picks, instead of the
 * one global install dir. The choice is REMEMBERED (config/dir-<id>, the same
 * file adopt writes), so later upgrades land in the same place. Testers asked
 * for this: one global drawer is not enough when a tool belongs in C: and
 * everything else belongs in SYS:Programs. */
static void action_install_to(void)
{
    struct FileRequester *fr;
    char cur[256], cmd[420], verb[64];
    const char *id;
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    id = g_rowid[g_selected];
    if (!AslBase) { set_status("asl.library unavailable - use: amipkg install <id> DIR=<drawer>"); return; }
    amipkg_get_pkgdir(id, cur, sizeof cur);    /* start where it would go now */
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Install into which drawer?",
            ASLFR_DrawersOnly,   TRUE,
            ASLFR_InitialDrawer, (ULONG)cur,
            TAG_END);
    if (!fr) { set_status("Could not open the drawer requester."); return; }
    if (AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        snprintf(cmd, sizeof cmd, "%s install %s \"DIR=%s\"", cli_path(), id, fr->fr_Drawer);
        snprintf(verb, sizeof verb, "Install of '%s'", id);
        FreeAslRequest(fr);
        run_async(cmd, verb);
        return;
    }
    FreeAslRequest(fr);
}

/* Update just the selected package: the CLI has taken `upgrade <id>` since
 * 0.4, the GUI simply never offered it. */
static void action_update_selected(void)
{
    char cmd[300], verb[64];
    const char *id;
    if (g_busy) { set_status("An operation is already running."); return; }
    if (g_selected < 0 || (size_t)g_selected >= g_nrows) { set_status("Select a package first."); return; }
    id = g_rowid[g_selected];
    snprintf(cmd, sizeof cmd, "%s upgrade %s", cli_path(), id);
    snprintf(verb, sizeof verb, "Update of '%s'", id);
    set_status("Updating...");
    run_async(cmd, verb);
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
                 "Install '%s'?\n\n%s\nDownloads are SHA-256-verified.",
                 id, plan);
    else
        snprintf(body, sizeof body,
                 "Install '%s'?\n\nDownloads + verifies the archive and\ninstalls it (with any dependencies).",
                 id);
    if (!req_confirm("Install Package", body, "Install|Cancel")) { set_status("Install cancelled."); return; }
    snprintf(body, sizeof body, "Installing %s...", id);
    set_status(body);
    snprintf(cmd, sizeof cmd, "%s install %s", cli_path(), id);
    snprintf(verb, sizeof verb, "Install of '%s'", id);
    run_async(cmd, verb);
}

/* "Run" - launch the selected installed package. The executable is found from
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
        static char script[900];
        size_t dl = (size_t)(strlen(best) - strlen(bestname));
        if (dl >= sizeof drawer) dl = sizeof drawer - 1;
        memcpy(drawer, best, dl); drawer[dl] = '\0';
        /* Execute the binary DIRECTLY in the (already asynchronous) script -
         * no inner Run - with a combined <> redirection to an AUTO console:
         * GUI apps never touch stdio, so no window appears; CONSOLE apps
         * (MCAmiga XPE report - "Run does nothing") get a real interactive
         * window that stays until closed. Stack 100000 covers ports that
         * assume a big-stack icon launch (default 4K would crash them). */
        snprintf(script, sizeof script,
                 "FailAt 9999\nStack 100000\nCD \"%s\"\n"
                 "\"%s\" <> \"CON:40/40/620/360/%.40s (amipkg Run)/AUTO/CLOSE/WAIT\"\n",
                 drawer, bestname, bestname);
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
#define UI_ROWS   19   /* tall enough for the 9-button column + more packages */

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
     * border - otherwise the header sits under the title bar ("too high"). */
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

    /* View cycle (Installed / Available) - top-RIGHT, directly above the button
     * column (its "Installed"/"Available" text is self-describing, no label). */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = bx; ng.ng_TopEdge = findY;
    ng.ng_Width = UI_BTN_W; ng.ng_Height = findH;
    ng.ng_GadgetText = NULL; ng.ng_Flags = 0;
    ng.ng_TextAttr = ta; ng.ng_VisualInfo = g_vi; ng.ng_GadgetID = GID_VIEW;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)g_viewlabels, GTCY_Active, g_view, TAG_END);
    g_gads[GID_VIEW] = gad;

    /* Find box - filters the list by a case-insensitive id substring. */
    memset(&ng, 0, sizeof ng);
    ng.ng_LeftEdge = wleft + UI_MARGIN + 44; ng.ng_TopEdge = findY;
    ng.ng_Width = UI_LV_W - 44; ng.ng_Height = findH;
    ng.ng_GadgetText = (UBYTE *)"Find:"; ng.ng_Flags = PLACETEXT_LEFT;
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

    /* Listview of packages - rendered in the SYSTEM DEFAULT font, which is
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
            {(UBYTE *)"Update",        GID_UPDATE1, 1},
            {(UBYTE *)"Info",          GID_INFO,    1},
            {(UBYTE *)"Install",       GID_INSTALL, 1},
            {(UBYTE *)"Run",           GID_RUN,     1},
            {(UBYTE *)"Remove",        GID_REMOVE,  1},
            {(UBYTE *)"Adopt...",      GID_ADOPT,   1},
            {(UBYTE *)"Refresh",       GID_REFRESH, 0},
        };
        int i;
        for (i = 0; i < 9; i++) {
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

    /* Progress line - the running operation's last output line. */
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
        char *pos = read_file(amipkg_data_path("config/winpos"));
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
         * text on a standard WB scheme) - without this Intuition falls back
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
        FILE *f = fopen(amipkg_data_path("config/winpos"), "w");
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
    if (menuNum == MENU_APP) {
        if (itemNum == APP_ABOUT) action_about();
        else if (itemNum == APP_DOCS) {
            open_docs();
            set_status("Opening the documentation (MultiView)...");
        }
        else if (itemNum == APP_QUIT) return 1;
    } else if (menuNum == MENU_PACKAGE) {
        if (itemNum == PKG_UPDATECAT) action_update_catalog();
        else if (itemNum == PKG_CHECK) action_check();
        else if (itemNum == PKG_UPGRADE) action_upgrade();
        else if (itemNum == PKG_INFO) action_info();
        else if (itemNum == PKG_INSTALL) action_install();
        else if (itemNum == PKG_INSTALLTO) action_install_to();
        else if (itemNum == PKG_UPDATE1) action_update_selected();
        else if (itemNum == PKG_ADOPT) action_adopt();
        else if (itemNum == PKG_SUBMIT) submit_form();
        else if (itemNum == PKG_REMOVE) action_remove();
        else if (itemNum == PKG_REFRESH) action_refresh();
    } else if (menuNum == MENU_SETTINGS) {
        if (itemNum == SET_DIR) action_set_dir();
        else if (itemNum == SET_REPOS) repo_manager();
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
                case GID_ADOPT: action_adopt(); break;
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
                case GID_UPDATE1: action_update_selected(); break;
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
 * StackSwap idiom as the CLI - globals carry the result across the swap. */
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
