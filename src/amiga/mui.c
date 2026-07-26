/*
 * mui.c — amipkg-mui, the MUI front-end for the AmigaPKG package manager.
 *
 * The polished alternative to the zero-dependency GadTools GUI (gui.c):
 * resizable window, real list columns, description pane — the look Amiga
 * users know from YAM/IBrowse. Needs MUI 3.8+ (muimaster.library v19);
 * `amipkg install mui38` bootstraps it. Feature-parity with gui.c: view/
 * category/sort filters, Find, catalog update, check/update-all, install
 * with a DRYRUN plan in the confirmation, Run, remove, Install Drawer.
 *
 * Mutations shell out to the amipkg CLI exactly like gui.c so it stays the
 * single source of truth: operations run DETACHED (SYS_Asynch Execute of a
 * RAM: script writing output + a return-code sentinel), and a 250 ms
 * timer.device heartbeat streams the output tail into the progress line.
 *
 * Headers: vendor/mui/include (MUI 3.8 developer kit, freely distributable;
 * see vendor/mui/README). Link: -lamiga (DoMethod + HookEntry).
 *
 * PARITY RULE: gui.c (GadTools) and mui.c are maintained in LOCKSTEP — every
 * user-facing feature lands in BOTH front-ends in the same change. Neither is
 * the "lesser" GUI; GadTools covers stock systems, MUI covers preference.
 */

#ifdef __amigaos__

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <libraries/asl.h>
#include <libraries/mui.h>
#include <utility/hooks.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/asl.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/store.h"
#include "../core/aindex.h"
#include "../core/aver.h"

static const char verstag[] __attribute__((used)) = "$VER: amipkg-mui 0.4.2 (26.7.2026)";

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

struct Library *MUIMasterBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct Library *AslBase = NULL;
struct Library *UtilityBase = NULL;   /* MUI notification tags use utility */

/* Guaranteed stack (same rationale as the CLI/GadTools GUI). */
#define STACK_BYTES (128UL * 1024UL)

/* ---- IDs ------------------------------------------------------------------ */
enum {
    ID_UPDATE = 1, ID_CHECK, ID_UPALL, ID_INFO, ID_INSTALL, ID_RUN,
    ID_REMOVE, ID_REFRESH, ID_SETDIR, ID_ABOUT, ID_ADOPT, ID_DOCS,
    ID_VIEW, ID_CAT, ID_SORT, ID_FIND, ID_SELECT, ID_DCLICK
};

/* ---- model (mirrors gui.c) ------------------------------------------------ */

typedef struct {
    char id[64];
    char flag[4];        /* "*" installed marker (Available view) */
    char version[16];
    char status[14];
    char desc[100];
} Row;

static Row    g_rows[MAX_PKGS];
static size_t g_nrows = 0;

enum { VIEW_INSTALLED = 0, VIEW_AVAILABLE = 1 };
static const char *g_viewlabels[] = { "Installed", "Available", NULL };
static int g_view = VIEW_AVAILABLE;      /* MUI users browse: start in Available */

#define MAX_CATS 16
static char        g_catnames[MAX_CATS][24];
static const char *g_catlabels[MAX_CATS + 2];
static size_t      g_ncats = 0;
static int         g_cat_sel = 0;

static const char *g_sortlabels[] = { "By Name", "Newest", NULL };
static int g_sort_recent = 0;

static char g_filter[64] = "";

/* ---- MUI objects ---------------------------------------------------------- */

static Object *app, *win, *lv, *lst, *str_find, *cyc_view, *cyc_cat, *cyc_sort;
static Object *txt_status, *txt_progress, *txt_desc;
static Object *bt_update, *bt_check, *bt_upall, *bt_info, *bt_install,
              *bt_run, *bt_remove, *bt_refresh;

/* ---- async op state (same protocol as gui.c) ------------------------------ */

static int  g_busy = 0;
static char g_busy_verb[48];
static long g_busy_ticks = 0;
#define ASYNC_OUT    "RAM:amipkg-mui.out"
#define ASYNC_DONE   "RAM:amipkg-mui.done"
#define ASYNC_SCRIPT "RAM:amipkg-mui.script"

/* Crash-hunt breadcrumbs (tester machines Guru with no data): every startup
 * phase appends one line to RAM:amipkg-mui.trace, opened+closed per line so
 * it survives a crash. `Type RAM:amipkg-mui.trace` after a crash and report
 * the LAST line - it names the exact phase that died. Cheap (startup only). */
static void trace(const char *msg)
{
    /* Prefer the HOME DRAWER (survives the reboot a hard Guru forces - RAM:
     * is wiped, which cost us the first trace round); RAM: only as the
     * fallback before the assign bridge has run. */
    FILE *f = fopen("AMIPKG:amipkg-mui.trace", "a");
    if (!f) f = fopen("RAM:amipkg-mui.trace", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    printf("amipkg-mui: %s\n", msg);
}

/* ---- helpers -------------------------------------------------------------- */


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
{ static char b[160]; strncpy(b, s, sizeof b - 1); b[sizeof b - 1] = 0;
  set(txt_status, MUIA_Text_Contents, (ULONG)b); }

static void set_progress(const char *s)
{ static char b[160]; strncpy(b, s, sizeof b - 1); b[sizeof b - 1] = 0;
  set(txt_progress, MUIA_Text_Contents, (ULONG)b); }

static void set_desc(const char *s)
{ static char b[120]; strncpy(b, s, sizeof b - 1); b[sizeof b - 1] = 0;
  set(txt_desc, MUIA_Text_Contents, (ULONG)b); }

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (!nl) return 1;
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

static int id_installed(const rcpt_installed *inst, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(inst[i].id, id) == 0) return 1;
    return 0;
}

static const char *shown_version(const char *v)
{ return (!v[0] || (v[0] == '-' && !v[1])) ? "-" : v; }

static void note_category(const char *cat)
{
    size_t i;
    if (!cat[0]) return;
    for (i = 0; i < g_ncats; i++) if (strcmp(g_catnames[i], cat) == 0) return;
    if (g_ncats >= MAX_CATS) return;
    strncpy(g_catnames[g_ncats], cat, sizeof g_catnames[0] - 1);
    g_catnames[g_ncats][sizeof g_catnames[0] - 1] = 0;
    g_ncats++;
}

static int cmp_recent(const void *a, const void *b)
{
    const aidx_entry *ea = *(const aidx_entry * const *)a;
    const aidx_entry *eb = *(const aidx_entry * const *)b;
    int ha = ea->added[0] != 0, hb = eb->added[0] != 0;
    if (ha != hb) return hb - ha;
    if (ha) { int c = strcmp(eb->added, ea->added); if (c) return c; }
    return strcmp(ea->id, eb->id);
}

/* ---- list display hook ---------------------------------------------------- */

/* HookEntry (amiga.lib) moves the register args onto the stack for us:
 * (hook, object=a2, message=a1). For a List display hook: a2 = char *array[],
 * a1 = the entry (NULL for the title row). */
static ULONG disp_func(struct Hook *h, char **array, Row *r)
{
    (void)h;
    if (r) {
        array[0] = r->flag;
        array[1] = r->id;
        array[2] = r->version;
        array[3] = r->status;
    } else {
        array[0] = (char *)"";
        array[1] = (char *)"\033bPackage";
        array[2] = (char *)"\033bVersion";
        array[3] = (char *)"\033bStatus";
    }
    return 0;
}
static struct Hook disp_hook;

/* ---- model rebuild -------------------------------------------------------- */

static void rebuild_list(void)
{
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst, i;

    set(lst, MUIA_List_Quiet, TRUE);
    DoMethod(lst, MUIM_List_Clear);
    g_nrows = 0;
    ninst = load_installed(inst, MAX_PKGS);

    if (g_view == VIEW_INSTALLED) {
        for (i = 0; i < ninst && g_nrows < MAX_PKGS; i++) {
            Row *r = &g_rows[g_nrows];
            if (!contains_ci(inst[i].id, g_filter)) continue;
            strncpy(r->id, inst[i].id, sizeof r->id - 1); r->id[sizeof r->id - 1] = 0;
            strcpy(r->flag, "");
            strncpy(r->version, shown_version(inst[i].version), sizeof r->version - 1);
            r->version[sizeof r->version - 1] = 0;
            strcpy(r->status, "installed");
            r->desc[0] = 0;
            DoMethod(lst, MUIM_List_InsertSingle, (ULONG)r, MUIV_List_Insert_Bottom);
            g_nrows++;
        }
    } else {
        aidx_index idx;
        char *text = read_file(AMIPKG_INDEX_PATH);
        if (text && aidx_parse(text, &idx) == 0) {
            static const aidx_entry *order[MAX_PKGS * 2];
            size_t norder = 0;
            const char *want_cat = g_cat_sel > 0 && (size_t)(g_cat_sel - 1) < g_ncats
                                   ? g_catnames[g_cat_sel - 1] : NULL;
            g_ncats = 0;
            for (i = 0; i < idx.count; i++) note_category(idx.entries[i].category);
            for (i = 0; i < idx.count && norder < sizeof order / sizeof order[0]; i++) {
                const aidx_entry *e = &idx.entries[i];
                if (!contains_ci(e->id, g_filter) && !contains_ci(e->name, g_filter)) continue;
                if (want_cat && strcmp(e->category, want_cat) != 0) continue;
                order[norder++] = e;
            }
            if (g_sort_recent) qsort(order, norder, sizeof order[0], cmp_recent);
            for (i = 0; i < norder && g_nrows < MAX_PKGS; i++) {
                const aidx_entry *e = order[i];
                Row *r = &g_rows[g_nrows];
                int ins = id_installed(inst, ninst, e->id);
                strncpy(r->id, e->id, sizeof r->id - 1); r->id[sizeof r->id - 1] = 0;
                strcpy(r->flag, ins ? "*" : "");
                strncpy(r->version, shown_version(e->version), sizeof r->version - 1);
                r->version[sizeof r->version - 1] = 0;
                strcpy(r->status, ins ? "installed" : "");
                strncpy(r->desc, e->description, sizeof r->desc - 1);
                r->desc[sizeof r->desc - 1] = 0;
                DoMethod(lst, MUIM_List_InsertSingle, (ULONG)r, MUIV_List_Insert_Bottom);
                g_nrows++;
            }
            aidx_free(&idx);
        } else {
            set_status("No catalog yet - click Update Catalog (needs network).");
        }
        if (text) free(text);
    }
    set(lst, MUIA_List_Quiet, FALSE);
    {
        char b[64];
        snprintf(b, sizeof b, "%lu package%s %s", (unsigned long)g_nrows,
                 g_nrows == 1 ? "" : "s",
                 g_view == VIEW_AVAILABLE ? "available" : "installed");
        set_status(b);
    }
    set_desc("");
}

static Row *selected_row(void)
{
    Row *r = NULL;
    DoMethod(lst, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&r);
    return r;
}

static int selection_installed(void)
{
    static rcpt_installed inst[MAX_PKGS];
    Row *r = selected_row();
    if (!r) return 0;
    return id_installed(inst, load_installed(inst, MAX_PKGS), r->id);
}

static void update_action_state(void)
{
    Row *r = selected_row();
    int busy = g_busy;
    int inst = r && selection_installed();
    set(bt_info,    MUIA_Disabled, r ? FALSE : TRUE);
    set(bt_install, MUIA_Disabled, (!busy && r && g_view == VIEW_AVAILABLE) ? FALSE : TRUE);
    set(bt_remove,  MUIA_Disabled, (!busy && inst) ? FALSE : TRUE);
    set(bt_run,     MUIA_Disabled, (!busy && inst) ? FALSE : TRUE);
    set(bt_update,  MUIA_Disabled, busy ? TRUE : FALSE);
    set(bt_upall,   MUIA_Disabled, busy ? TRUE : FALSE);
    set(bt_refresh, MUIA_Disabled, busy ? TRUE : FALSE);
}

/* ---- async ops (same RAM:-file protocol as gui.c) ------------------------- */

static void tail_line(const char *path, char *out, size_t outsize)
{
    static char buf[4096];
    BPTR fh;
    LONG n;
    out[0] = 0;
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_CURRENT);
    if (n > (LONG)sizeof buf - 1) Seek(fh, -(LONG)(sizeof buf - 1), OFFSET_END);
    else                          Seek(fh, 0, OFFSET_BEGINNING);
    n = Read(fh, buf, sizeof buf - 1);
    Close(fh);
    if (n <= 0) return;
    buf[n] = 0;
    {
        char *last = buf, *p;
        for (p = buf; *p; p++)
            if (*p == '\n') { *p = 0; if (*(p + 1)) last = p + 1; }
        strncpy(out, last, outsize - 1);
        out[outsize - 1] = 0;
    }
}

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
    strncpy(g_busy_verb, verb, sizeof g_busy_verb - 1);
    g_busy_verb[sizeof g_busy_verb - 1] = 0;
    g_busy = 1;
    g_busy_ticks = 0;
    set_progress("Working...");
    update_action_state();
    return 1;
}

static void poll_async(void)
{
    char line[160];
    BPTR fh;
    if (!g_busy) return;
    g_busy_ticks++;
    if ((g_busy_ticks % 2) == 0) {          /* ~2x/s at a 250 ms heartbeat */
        tail_line(ASYNC_OUT, line, sizeof line);
        if (line[0]) set_progress(line);
    }
    fh = Open((STRPTR)ASYNC_DONE, MODE_OLDFILE);
    if (fh) {
        char rcbuf[16];
        LONG n = Read(fh, rcbuf, sizeof rcbuf - 1);
        long rc = 0;
        Close(fh);
        if (n > 0) { rcbuf[n] = 0; rc = atol(rcbuf); }
        tail_line(ASYNC_OUT, line, sizeof line);
        if (line[0]) set_progress(line);
        g_busy = 0;
        DeleteFile((STRPTR)ASYNC_DONE);
        DeleteFile((STRPTR)ASYNC_SCRIPT);
        rebuild_list();
        {
            char st[96];
            if (rc == 0) snprintf(st, sizeof st, "%s finished.", g_busy_verb);
            else         snprintf(st, sizeof st, "%s FAILED (rc %ld) - see below.", g_busy_verb, rc);
            set_status(st);
        }
        update_action_state();
    } else if (g_busy_ticks > 4L * 60 * 30) {   /* ~30 min watchdog */
        g_busy = 0;
        set_status("Operation timed out - check a Shell.");
        update_action_state();
    }
}

static long run_sync_capture(const char *cmd, char *buf, long bufsize)
{
    char full[300];
    BPTR fh;
    LONG n = 0;
    snprintf(full, sizeof full, "%s >RAM:amipkg-mui-plan.out", cmd);
    SystemTags((STRPTR)full, TAG_DONE);
    fh = Open((STRPTR)"RAM:amipkg-mui-plan.out", MODE_OLDFILE);
    if (fh) { n = Read(fh, buf, bufsize - 1); Close(fh); }
    DeleteFile((STRPTR)"RAM:amipkg-mui-plan.out");
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

/* ---- actions -------------------------------------------------------------- */

static int req_yesno(const char *title, const char *gadgets, const char *body)
{
    return MUI_Request(app, win, 0, (char *)title, (char *)gadgets, "%s", (ULONG)body);
}

static void action_update_catalog(void)
{
    set_status("Updating catalog... (needs a TCP/IP stack up)");
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s update", cli_path());
    run_async(cmd, "Catalog update");
}

static void action_check(void)
{
    set_status("Checking for updates...");
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s check", cli_path());
    run_async(cmd, "Update check");
}

static void action_upall(void)
{
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!req_yesno("Update All", "_Update|_Cancel",
                   "Upgrade every out-of-date package?\n\n"
                   "Downloads + reinstalls the newer versions\n(needs a TCP/IP stack up)."))
        { set_status("Update cancelled."); return; }
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s upgrade", cli_path());
    run_async(cmd, "Update All");
}

static void action_info(void)
{
    aidx_index idx;
    const aidx_entry *e;
    char *text;
    static char msg[1024];
    Row *r = selected_row();
    if (!r) { set_status("Select a package first."); return; }
    text = read_file(AMIPKG_INDEX_PATH);
    if (!text) { set_status("No catalog yet - click Update Catalog (needs network)."); return; }
    if (aidx_parse(text, &idx) != 0) { free(text); set_status("Seeded index unreadable."); return; }
    free(text);
    e = aidx_find(&idx, r->id);
    if (!e) {
        snprintf(msg, sizeof msg, "\033b%s\033n\n\nInstalled on this system\n(not in the catalog).", r->id);
    } else {
        size_t u = 0, k;
        u += (size_t)snprintf(msg + u, sizeof msg - u, "\033b%s\033n - %s\n", e->id, e->name);
        if (e->description[0])
            u += (size_t)snprintf(msg + u, sizeof msg - u, "\n%s\n", e->description);
        u += (size_t)snprintf(msg + u, sizeof msg - u, "\ncategory: %s\nversion: %s\n",
                              e->category, shown_version(e->version));
        if (e->added[0])
            u += (size_t)snprintf(msg + u, sizeof msg - u, "added: %s\n", e->added);
        if (e->dep_count) {
            u += (size_t)snprintf(msg + u, sizeof msg - u, "needs:");
            for (k = 0; k < e->dep_count && u < sizeof msg - 70; k++)
                u += (size_t)snprintf(msg + u, sizeof msg - u, " %s", e->deps[k].id);
            u += (size_t)snprintf(msg + u, sizeof msg - u, "\n");
        }
        u += (size_t)snprintf(msg + u, sizeof msg - u, "install: %s",
                              e->has_recipe ? "portable recipe" : "generic / build-time");
    }
    aidx_free(&idx);
    MUI_Request(app, win, 0, (char *)"Package Info", (char *)"_OK", "%s", (ULONG)msg);
}

static void action_install(void)
{
    char cmd[256], verb[48];
    static char plan[1200], body[1500];
    Row *r = selected_row();
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!r) { set_status("Select a package first."); return; }
    set_status("Resolving...");
    snprintf(cmd, sizeof cmd, "%s install %s DRYRUN", cli_path(), r->id);
    run_sync_capture(cmd, plan, sizeof plan);
    if (plan[0])
        snprintf(body, sizeof body,
                 "Install \033b%s\033n?\n\n%s\nDownloads are SHA-256-verified.",
                 r->id, plan);
    else
        snprintf(body, sizeof body,
                 "Install \033b%s\033n?\n\nDownloads + verifies + installs it\n(with any dependencies).", r->id);
    if (!req_yesno("Install Package", "_Install|_Cancel", body))
        { set_status("Install cancelled."); return; }
    snprintf(cmd, sizeof cmd, "%s install %s", cli_path(), r->id);
    snprintf(verb, sizeof verb, "Install of '%s'", r->id);
    set_status("Installing...");
    run_async(cmd, verb);
}

static void action_remove(void)
{
    char cmd[256], body[256], verb[48];
    Row *r = selected_row();
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!r) { set_status("Select a package first."); return; }
    snprintf(body, sizeof body,
             "Remove \033b%s\033n?\n\nIts files, User-Startup entries and\nreceipts are cleanly removed.", r->id);
    if (!req_yesno("Remove Package", "_Remove|_Cancel", body))
        { set_status("Remove cancelled."); return; }
    snprintf(cmd, sizeof cmd, "%s remove %s FORCE", cli_path(), r->id);
    snprintf(verb, sizeof verb, "Remove of '%s'", r->id);
    set_status("Removing...");
    run_async(cmd, verb);
}

/* Launch an installed program: same receipt heuristic as gui.c. */
static void action_run(void)
{
    static rcpt_file files[MAX_FILES];
    size_t nfiles, i;
    char best[256] = "", bestname[256] = "";
    long best_size = -1;
    int best_is_name_match = 0;
    Row *r = selected_row();
    if (!r) { set_status("Select a package first."); return; }
    nfiles = load_files_for(r->id, files, MAX_FILES);
    if (!nfiles) { set_status("No file inventory - run it from Workbench."); return; }
    for (i = 0; i < nfiles; i++) {
        const char *path = files[i].path;
        const char *base = strrchr(path, '/');
        FILE *f;
        unsigned char magic[4];
        long size = 0;
        int name_match;
        base = base ? base + 1 : (strrchr(path, ':') ? strrchr(path, ':') + 1 : path);
        if (strlen(base) > 5 && strcmp(base + strlen(base) - 5, ".info") == 0) continue;
        f = fopen(path, "rb");
        if (!f) continue;
        if (fread(magic, 1, 4, f) != 4
            || magic[0] != 0 || magic[1] != 0 || magic[2] != 3 || magic[3] != 0xF3) {
            fclose(f); continue;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
        name_match = strncasecmp(base, r->id, strlen(r->id)) == 0;
        if ((name_match && !best_is_name_match)
            || (name_match == best_is_name_match && size > best_size)) {
            strncpy(best, path, sizeof best - 1); best[sizeof best - 1] = 0;
            strncpy(bestname, base, sizeof bestname - 1); bestname[sizeof bestname - 1] = 0;
            best_size = size;
            best_is_name_match = name_match;
        }
    }
    if (!best[0]) { set_status("No executable found in this package."); return; }
    {
        char drawer[256], b[300];
        BPTR fh;
        static char script[600];
        size_t dl = (size_t)(strlen(best) - strlen(bestname));
        if (dl >= sizeof drawer) dl = sizeof drawer - 1;
        memcpy(drawer, best, dl); drawer[dl] = 0;
        snprintf(script, sizeof script, "CD \"%s\"\nRun >NIL: \"%s\"\n", drawer, bestname);
        fh = Open((STRPTR)"RAM:amipkg-mui-run.script", MODE_NEWFILE);
        if (!fh) { set_status("Could not write the RAM: launch script."); return; }
        Write(fh, script, (LONG)strlen(script));
        Close(fh);
        {
            BPTR in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
            BPTR out = Open((STRPTR)"NIL:", MODE_NEWFILE);
            if (SystemTags((STRPTR)"Execute RAM:amipkg-mui-run.script",
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

/* "Adopt Existing..." — parity with gui.c (jdb78's idea). */
static void action_adopt(void)
{
    struct FileRequester *fr;
    char cmd[400], verb[48];
    Row *r = selected_row();
    if (g_busy) { set_status("An operation is already running."); return; }
    if (!r) { set_status("Select the package you already have."); return; }
    if (!AslBase) { set_status("asl.library unavailable - use: amipkg adopt <id> <drawer>"); return; }
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,   (ULONG)"Where is it installed? (pick its drawer)",
            ASLFR_DrawersOnly, TRUE,
            TAG_END);
    if (!fr) { set_status("Could not open the drawer requester."); return; }
    if (AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        snprintf(cmd, sizeof cmd, "%s adopt %s \"%s\"", cli_path(), r->id, fr->fr_Drawer);
        snprintf(verb, sizeof verb, "Adopt of '%s'", r->id);
        set_status("Adopting...");
        run_async(cmd, verb);
    }
    FreeAslRequest(fr);
}

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
        if (amipkg_set_installdir((const char *)fr->fr_Drawer) == 0) {
            snprintf(b, sizeof b, "Install drawer: %s", fr->fr_Drawer);
            set_status(b);
        } else set_status("Could not save the install drawer.");
    }
    FreeAslRequest(fr);
}

static void action_about(void)
{
    MUI_Request(app, win, 0, (char *)"About amipkg", (char *)"_OK",
        "\033bamipkg-mui 0.4\033n\n\n"
        "The AmigaPKG package manager for AmigaOS 3.x.\n"
        "Browse, install, update, and remove software\n"
        "from the signed AmigaPKG catalog.\n\n"
        "(c) 2026 Thomas Luebker\n\n"
        "Catalog + package submissions:\n"
        "https://github.com/thomas-luebker/amiga-pkg");
}

/* ---- UI ------------------------------------------------------------------- */

static int build_app(void)
{
    disp_hook.h_Entry    = (HOOKFUNC)HookEntry;
    disp_hook.h_SubEntry = (HOOKFUNC)disp_func;
    trace("build_app: parsing catalog for categories");

    /* Category labels for the cycle: harvested once from the seeded index. */
    {
        aidx_index idx;
        char *text = read_file(AMIPKG_INDEX_PATH);
        size_t i;
        if (text && aidx_parse(text, &idx) == 0) {
            for (i = 0; i < idx.count; i++) note_category(idx.entries[i].category);
            aidx_free(&idx);
        }
        if (text) free(text);
        g_catlabels[0] = "All categories";
        for (i = 0; i < g_ncats; i++) g_catlabels[i + 1] = g_catnames[i];
        g_catlabels[g_ncats + 1] = NULL;
    }
    trace("build_app: categories done, creating MUI application tree");

    app = ApplicationObject,
        MUIA_Application_Title,       (ULONG)"amipkg",
        MUIA_Application_Version,     (ULONG)&verstag[1],
        MUIA_Application_Author,      (ULONG)"Thomas Luebker",
        MUIA_Application_Copyright,   (ULONG)"(c) 2026 Thomas Luebker",
        MUIA_Application_Description, (ULONG)"AmigaPKG package manager",
        MUIA_Application_Base,        (ULONG)"AMIPKG",

#ifndef NO_MENUS
        MUIA_Application_Menustrip, (ULONG)(MenustripObject,
            MUIA_Family_Child, MenuObjectT("Project"),
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"About...",
                    MUIA_UserData, ID_ABOUT, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Documentation...",
                    MUIA_Menuitem_Shortcut, (ULONG)"?",
                    MUIA_UserData, ID_DOCS, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Quit",
                    MUIA_Menuitem_Shortcut, (ULONG)"Q",
                    MUIA_UserData, MUIV_Application_ReturnID_Quit, End,
            End,
            MUIA_Family_Child, MenuObjectT("Package"),
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Update Catalog",
                    MUIA_Menuitem_Shortcut, (ULONG)"A",
                    MUIA_UserData, ID_UPDATE, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Check Updates",
                    MUIA_Menuitem_Shortcut, (ULONG)"C",
                    MUIA_UserData, ID_CHECK, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Update All",
                    MUIA_Menuitem_Shortcut, (ULONG)"U",
                    MUIA_UserData, ID_UPALL, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Info",
                    MUIA_Menuitem_Shortcut, (ULONG)"I",
                    MUIA_UserData, ID_INFO, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Install",
                    MUIA_Menuitem_Shortcut, (ULONG)"N",
                    MUIA_UserData, ID_INSTALL, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Run",
                    MUIA_UserData, ID_RUN, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Adopt Existing...",
                    MUIA_UserData, ID_ADOPT, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Remove",
                    MUIA_Menuitem_Shortcut, (ULONG)"R",
                    MUIA_UserData, ID_REMOVE, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Refresh",
                    MUIA_Menuitem_Shortcut, (ULONG)"F",
                    MUIA_UserData, ID_REFRESH, End,
                MUIA_Family_Child, MenuitemObject, MUIA_Menuitem_Title, (ULONG)"Install Drawer...",
                    MUIA_Menuitem_Shortcut, (ULONG)"D",
                    MUIA_UserData, ID_SETDIR, End,
            End,
        End),
#endif

        SubWindow, win = WindowObject,
            MUIA_Window_Title, (ULONG)"amipkg - AmigaPKG Package Manager",
            MUIA_Window_ID,    MAKE_ID('A','P','K','G'),
            WindowContents, VGroup,

                /* filter row */
                Child, HGroup,
                    Child, Label2("Find"),
                    Child, str_find = StringObject, StringFrame,
                        MUIA_String_MaxLen, 63,
                        MUIA_CycleChain, 1,
                    End,
                    Child, cyc_view = CycleObject,
                        MUIA_Cycle_Entries, (ULONG)g_viewlabels,
                        MUIA_Cycle_Active,  g_view,
                        MUIA_CycleChain, 1,
                    End,
                    Child, cyc_cat = CycleObject,
                        MUIA_Cycle_Entries, (ULONG)g_catlabels,
                        MUIA_CycleChain, 1,
                    End,
                    Child, cyc_sort = CycleObject,
                        MUIA_Cycle_Entries, (ULONG)g_sortlabels,
                        MUIA_CycleChain, 1,
                    End,
                End,

                /* package list */
                Child, lv = ListviewObject,
                    /* WEIGHTs only - MIW/MAW are PERCENT clamps, and
                     * "MAW=2" made column 0's minimum exceed its maximum
                     * on big-font RTG Workbenches: MUI then REFUSES to
                     * open the window (tester trace; UAE's topaz-8 just
                     * squeaked by, which is why it worked there). */
                    MUIA_Listview_List, lst = ListObject, InputListFrame,
                        MUIA_List_Format,      (ULONG)"WEIGHT=1,WEIGHT=100,WEIGHT=25,WEIGHT=30",
                        MUIA_List_Title,       TRUE,
                        MUIA_List_DisplayHook, (ULONG)&disp_hook,
                    End,
                End,

                /* description pane */
                Child, txt_desc = TextObject, TextFrame,
                    MUIA_Text_Contents, (ULONG)"",
                    MUIA_Background, MUII_TextBack,
                End,

                /* buttons */
                Child, HGroup,
                    Child, bt_update  = SimpleButton("Update _Catalog"),
                    Child, bt_check   = SimpleButton("Chec_k Updates"),
                    Child, bt_upall   = SimpleButton("Update _All"),
                    Child, bt_refresh = SimpleButton("Re_fresh"),
                End,
                Child, HGroup,
                    Child, bt_info    = SimpleButton("_Info"),
                    Child, bt_install = SimpleButton("I_nstall"),
                    Child, bt_run     = SimpleButton("_Run"),
                    Child, bt_remove  = SimpleButton("Re_move"),
                End,

                /* status + progress */
                Child, txt_status = TextObject, TextFrame,
                    MUIA_Text_Contents, (ULONG)"Welcome to amipkg.",
                    MUIA_Background, MUII_TextBack,
                End,
                Child, txt_progress = TextObject, TextFrame,
                    MUIA_Text_Contents, (ULONG)"",
                    MUIA_Background, MUII_TextBack,
                End,
            End,
        End,
    End;
    if (!app) return 0;
    trace("build_app: application tree created, wiring notifications");

    /* notifications → ReturnIDs */
    DoMethod(app, MUIM_Notify, MUIA_Application_MenuAction, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_TriggerValue);
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(cyc_view, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_VIEW);
    DoMethod(cyc_cat, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_CAT);
    DoMethod(cyc_sort, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_SORT);
    DoMethod(str_find, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_FIND);
    DoMethod(lst, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_SELECT);
    DoMethod(lv, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_DCLICK);
    DoMethod(bt_update,  MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_UPDATE);
    DoMethod(bt_check,   MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_CHECK);
    DoMethod(bt_upall,   MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_UPALL);
    DoMethod(bt_refresh, MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_REFRESH);
    DoMethod(bt_info,    MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_INFO);
    DoMethod(bt_install, MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_INSTALL);
    DoMethod(bt_run,     MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_RUN);
    DoMethod(bt_remove,  MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_REMOVE);
    trace("build_app: notifications wired");
    return 1;
}

/* ---- main loop with a timer heartbeat ------------------------------------- */

static int gui_run(void)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    ULONG tsig = 0;
    int timer_ok = 0, rc = 20;

    trace("start (0.4)");
    amipkg_bridge_assigns();
    trace("assign bridge done");

    /* Self-heal a fresh `amipkg install mui38`: its User-Startup assigns only
     * take effect after a REBOOT, leaving a trap window where muimaster may
     * be reachable (stray copy) but the MUI: tree is not - windows then fail
     * and menustrips can Guru (tester report: t2 no-window + 8000 0008 on
     * t4). If MUI: is missing but the standard drawer exists, set the same
     * assigns the User-Startup block would, for THIS session. */
    {
        struct Process *pr = (struct Process *)FindTask(NULL);
        APTR oldwin = pr->pr_WindowPtr;
        BPTR l;
        pr->pr_WindowPtr = (APTR)-1;
        l = Lock((STRPTR)"MUI:", ACCESS_READ);
        if (l) UnLock(l);
        else {
            BPTR d = Lock((STRPTR)"SYS:Programs/MUI", ACCESS_READ);
            if (d && AssignLock((STRPTR)"MUI", d)) {
                BPTR libs = Lock((STRPTR)"MUI:Libs", ACCESS_READ);
                if (libs && !AssignAdd((STRPTR)"LIBS", libs)) UnLock(libs);
                trace("self-healed MUI: assigns from SYS:Programs/MUI (session-only; reboot makes them permanent)");
            } else if (d) UnLock(d);
        }
        pr->pr_WindowPtr = oldwin;
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    UtilityBase   = OpenLibrary((STRPTR)"utility.library", 37);
    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19);
    AslBase       = OpenLibrary((STRPTR)"asl.library", 37);
    if (!MUIMasterBase) {
        printf("amipkg-mui: MUI 3.8+ is required (muimaster.library v19).\n");
        printf("Install it with:  amipkg install mui38   (or use amipkg-gui).\n");
        goto out;
    }
    if (!IntuitionBase || !UtilityBase) goto out;
    {
        char b[120];
        int mui_assign = 0;
        struct Process *pr = (struct Process *)FindTask(NULL);
        APTR oldwin = pr->pr_WindowPtr;
        BPTR l;
        pr->pr_WindowPtr = (APTR)-1;
        l = Lock((STRPTR)"MUI:", ACCESS_READ);
        if (l) { mui_assign = 1; UnLock(l); }
        pr->pr_WindowPtr = oldwin;
        snprintf(b, sizeof b, "libraries open: muimaster v%d.%d, MUI: assign %s",
                 (int)MUIMasterBase->lib_Version, (int)MUIMasterBase->lib_Revision,
                 mui_assign ? "present" : "MISSING");
        trace(b);
        if (!mui_assign)
            trace("WARNING: no MUI: assign - incomplete MUI install? "
                  "(after `amipkg install mui38` you must REBOOT once)");
    }

    /* Timer heartbeat first (independent of the MUI tree). */
#ifndef NO_TIMER
    tport = CreateMsgPort();
    if (tport) {
        treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
        if (treq && OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
                               (struct IORequest *)treq, 0) == 0) {
            timer_ok = 1;
            tsig = 1UL << tport->mp_SigBit;
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0;
            treq->tr_time.tv_micro = 250000;
            SendIO((struct IORequest *)treq);
        }
    }
#endif
    trace("timer heartbeat set up");

    /* Build + open, with ONE self-heal retry: every pre-0.4.1 build ran with
     * garbage tag lists (the inline-varargs compiler bug) and MUI saves
     * application prefs on exit - a poisoned ENV:MUI/AMIPKG.cfg from those
     * runs can make even a correct build refuse to open its window. On
     * failure we delete OUR OWN prefs file and rebuild once. */
    {
        int attempt;
        ULONG opened = 0;
        for (attempt = 0; attempt < 2 && !opened; attempt++) {
            if (!build_app()) { printf("amipkg-mui: could not create the application.\n"); goto out; }
#ifndef NO_MODEL
            trace("rebuild_list: loading receipts + catalog into the list");
            rebuild_list();
            trace("rebuild_list done");
#else
            set_status("NO_MODEL variant - list deliberately empty.");
#endif
            update_action_state();
            trace("action state set, opening window");
            set(win, MUIA_Window_Open, TRUE);
            get(win, MUIA_Window_Open, &opened);
            if (!opened) {
                char b[200];
                snprintf(b, sizeof b,
                         "window did NOT open (attempt %d). muimaster v%d.%d; chip free %ld KB, total free %ld KB.",
                         attempt + 1,
                         (int)MUIMasterBase->lib_Version, (int)MUIMasterBase->lib_Revision,
                         (long)(AvailMem(MEMF_CHIP) / 1024), (long)(AvailMem(MEMF_ANY) / 1024));
                trace(b);
                if (attempt == 0) {
                    trace("removing this app's MUI prefs (possibly saved by an earlier broken build), retrying once");
                    MUI_DisposeObject(app); app = NULL;
                    DeleteFile((STRPTR)"ENV:MUI/AMIPKG.cfg");
                    DeleteFile((STRPTR)"ENVARC:MUI/AMIPKG.cfg");
                } else {
                    trace("still refused. Usual causes now: incomplete MUI install "
                          "(no MUI: tree - reboot after installing MUI), broken global "
                          "MUI prefs, or a full/too-deep screen.");
                }
            }
        }
        if (!opened) goto out;
    }

    trace("window is open, entering main loop");
    {
        int done = 0;
        long zeros = 0;
        while (!done) {
            ULONG sigs = 0;
            LONG rid = DoMethod(app, MUIM_Application_NewInput, (ULONG)&sigs);
            switch (rid) {
            case MUIV_Application_ReturnID_Quit: done = 1; break;
            case ID_VIEW: {
                ULONG v = 0; get(cyc_view, MUIA_Cycle_Active, &v);
                g_view = (int)v; rebuild_list(); update_action_state(); break; }
            case ID_CAT: {
                ULONG v = 0; get(cyc_cat, MUIA_Cycle_Active, &v);
                g_cat_sel = (int)v; rebuild_list(); update_action_state(); break; }
            case ID_SORT: {
                ULONG v = 0; get(cyc_sort, MUIA_Cycle_Active, &v);
                g_sort_recent = (int)v; rebuild_list(); update_action_state(); break; }
            case ID_FIND: {
                char *s = NULL; get(str_find, MUIA_String_Contents, &s);
                strncpy(g_filter, s ? s : "", sizeof g_filter - 1);
                g_filter[sizeof g_filter - 1] = 0;
                rebuild_list(); update_action_state(); break; }
            case ID_SELECT: {
                Row *r = selected_row();
                update_action_state();
                if (r) set_desc(r->desc[0] ? r->desc : r->id);
                break; }
            case ID_DCLICK:  action_info(); break;
            case ID_UPDATE:  action_update_catalog(); break;
            case ID_CHECK:   action_check(); break;
            case ID_UPALL:   action_upall(); break;
            case ID_REFRESH: rebuild_list(); update_action_state(); break;
            case ID_INFO:    action_info(); break;
            case ID_INSTALL: action_install(); break;
            case ID_RUN:     action_run(); break;
            case ID_REMOVE:  action_remove(); break;
            case ID_SETDIR:  action_set_dir(); break;
            case ID_ADOPT:   action_adopt(); break;
            case ID_ABOUT:   action_about(); break;
            case ID_DOCS:
                SystemTags((STRPTR)"Run >NIL: SYS:Utilities/MultiView AMIPKG:amipkg.guide", TAG_DONE);
                set_status("Opening the documentation (MultiView)...");
                break;
            default: break;
            }
            if (done) break;
            if (sigs) {
                zeros = 0;
                sigs = Wait(sigs | tsig | SIGBREAKF_CTRL_C);
                if (sigs & SIGBREAKF_CTRL_C) done = 1;
                if (timer_ok && (sigs & tsig)) {
                    while (GetMsg(tport)) ;
                    poll_async();
                    treq->tr_node.io_Command = TR_ADDREQUEST;
                    treq->tr_time.tv_secs = 0;
                    treq->tr_time.tv_micro = 250000;
                    SendIO((struct IORequest *)treq);
                }
            } else if (++zeros > 100) {
                /* Spin guard: an empty signal mask should be transient. One
                 * tester machine streamed zeros -> 100% CPU. Yield politely. */
                Delay(1);
            }
        }
    }
    set(win, MUIA_Window_Open, FALSE);
    trace("clean exit");
    rc = 0;

out:
    if (timer_ok) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (app) MUI_DisposeObject(app);
    if (AslBase) CloseLibrary(AslBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/* StackSwap wrapper (same idiom as the CLI + GadTools GUI). */
static struct StackSwapStruct g_sss;
static char *g_stk;
static int   g_rc;

int main(void)
{
    g_stk = (char *)AllocMem(STACK_BYTES, MEMF_ANY);
    if (!g_stk) return gui_run();
    g_sss.stk_Lower   = (APTR)g_stk;
    g_sss.stk_Upper   = (ULONG)g_stk + STACK_BYTES;
    g_sss.stk_Pointer = (APTR)((ULONG)g_stk + STACK_BYTES);
    StackSwap(&g_sss);
    g_rc = gui_run();
    StackSwap(&g_sss);
    FreeMem(g_stk, STACK_BYTES);
    return g_rc;
}

#else
int main(void) { return 0; }   /* host build: MUI GUI is Amiga-only */
#endif
