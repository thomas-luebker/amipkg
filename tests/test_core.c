/*
 * test_core.c — amipkg portable-core test suite (host-run via clang).
 *
 * The vectors deliberately MIRROR the AmigaPackageKit Swift tests
 * (PackageIndexTests / PackageOperationsTests / PackageReceiptsTests):
 * the two implementations of the shared contract must agree. The fixture
 * fixtures/packages.json is a REAL `pkgindex generate` artifact from the
 * full catalog.
 */

#include "../src/core/sha256.h"
#include "../src/core/aver.h"
#include "../src/core/receipts.h"
#include "../src/core/ajson.h"
#include "../src/core/aindex.h"
#include "../src/core/resolve.h"
#include "../src/core/arecipe.h"
#include "../src/core/arun.h"
#include "../src/core/ajson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void test_sha256(void)
{
    char hex[65];
    /* FIPS 180-2 vectors — same as testSHA256HexMatchesKnownVector (Swift). */
    sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256(abc)");
    sha256_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "sha256(empty)");
    /* Multi-block + streaming equivalence. */
    {
        char buf[1000];
        int i;
        sha256_ctx c;
        unsigned char digest[32];
        char hex2[65];
        for (i = 0; i < 1000; i++) buf[i] = (char)('a' + (i % 26));
        sha256_hex(buf, sizeof buf, hex);
        sha256_init(&c);
        for (i = 0; i < 1000; i += 7)
            sha256_update(&c, buf + i, (size_t)((1000 - i) < 7 ? (1000 - i) : 7));
        sha256_final(&c, digest);
        for (i = 0; i < 32; i++) sprintf(hex2 + i * 2, "%02x", digest[i]);
        CHECK(strcmp(hex, hex2) == 0, "streaming == one-shot");
    }
}

static void test_aver(void)
{
    /* Mirror of testVersionOrdering (Swift). */
    CHECK(aver_is_newer("47.115", "47.9"), "47.115 > 47.9");
    CHECK(aver_is_newer("2.6b", "2.6"), "2.6b > 2.6");
    CHECK(aver_is_newer("2.6b", "2.6a"), "2.6b > 2.6a");
    CHECK(aver_is_newer("1.2.1", "1.2"), "1.2.1 > 1.2");
    CHECK(aver_is_newer("2.10", "2.b"), "numeric beats text");
    CHECK(!aver_is_newer("1.2", "1.2"), "equal not newer");
    CHECK(!aver_is_newer("1.2", "1.3"), "downgrade not newer");
    CHECK(aver_compare("1.2B", "1.2b") == 0, "case-insensitive");
    CHECK(aver_compare("1-2", "1.2") == 0, "separator-agnostic");
    CHECK(!aver_is_newer("2.0", "-"), "unknown installed never updates");
    CHECK(!aver_is_newer("-", "1.0"), "unknown candidate never updates");
    CHECK(aver_is_unknown("-"), "dash unknown");
    CHECK(aver_is_unknown(""), "empty unknown");
    CHECK(!aver_is_unknown("1.0"), "1.0 known");
}

static void test_receipts(void)
{
    /* Mirror of testInstalledContentSortedAndRoundTrips + files digests. */
    rcpt_installed inst[8];
    rcpt_file files[8];
    rcpt_edit edits[8];
    char line[128];
    size_t n;

    n = rcpt_parse_installed("igame|-|0|0\nwbdock|2.4|3|0\n", inst, 8);
    CHECK(n == 2, "installed count");
    CHECK(strcmp(inst[0].id, "igame") == 0 && strcmp(inst[0].version, "-") == 0, "igame row");
    CHECK(strcmp(inst[1].id, "wbdock") == 0 && strcmp(inst[1].version, "2.4") == 0
          && inst[1].index_version == 3, "wbdock row");
    rcpt_format_installed_line(&inst[1], line, sizeof line);
    CHECK(strcmp(line, "wbdock|2.4|3|0") == 0, "installed line round-trip");

    n = rcpt_parse_files("DH0:C/Foo|0011\nDH0:Libs/foo.library\n", files, 8);
    CHECK(n == 2, "files count");
    CHECK(strcmp(files[0].path, "DH0:C/Foo") == 0 && strcmp(files[0].sha256, "0011") == 0,
          "file with digest");
    CHECK(files[1].sha256[0] == '\0', "bare path = no digest (must ask)");

    n = rcpt_parse_edits("S:User-Startup|MUI38|0.99\n", edits, 8);
    CHECK(n == 1 && strcmp(edits[0].overlay, "MUI38") == 0
          && strcmp(edits[0].target, "S:User-Startup") == 0, "edit row");

    CHECK(rcpt_parse_installed("", inst, 8) == 0, "empty installed");
}

static void test_ajson(void)
{
    aj_node *root = ajson_parse("{\"a\": [1, 2, {\"b\": \"x\\\"y\"}], \"n\": -5, \"t\": true, \"z\": null}");
    const aj_node *arr, *obj;
    CHECK(root != NULL, "parse ok");
    if (!root) return;
    arr = ajson_get(root, "a");
    CHECK(arr && arr->type == AJ_ARR && ajson_arr_len(arr) == 3, "array len");
    obj = arr ? arr->child->next->next : NULL;
    CHECK(obj && obj->type == AJ_OBJ, "nested obj");
    CHECK(obj && strcmp(ajson_get_str(obj, "b", ""), "x\"y") == 0, "escaped string");
    CHECK(ajson_get_num(root, "n", 0) == -5, "negative number");
    CHECK(ajson_get_num(root, "t", 0) == 1, "bool true");
    ajson_free(root);
    CHECK(ajson_parse("{\"unterminated\": ") == NULL, "syntax error -> NULL");
    CHECK(ajson_parse("[1,2,]") == NULL, "trailing comma -> NULL");
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void test_real_index(void)
{
    /* The REAL pkgindex artifact: 58 packages, 8 with deps, 6 providers. */
    char *json = read_file("tests/fixtures/packages.json");
    aidx_index idx;
    const aidx_entry *igame, *roadie;
    CHECK(json != NULL, "fixture readable");
    if (!json) return;
    CHECK(aidx_parse(json, &idx) == 0, "real index parses");
    free(json);
    CHECK(idx.count == 58, "58 packages");
    CHECK(idx.index_version == 1, "indexVersion");

    igame = aidx_find(&idx, "igame");
    CHECK(igame != NULL, "igame present");
    if (igame) {
        CHECK(igame->dep_count == 4, "igame has 4 deps");
        CHECK(igame->has_recipe, "igame has a recipe");
    }
    roadie = aidx_find(&idx, "roadie");
    CHECK(roadie != NULL && roadie->has_recipe, "roadie recipe");

    /* Resolution on the REAL index — mirror of
     * testDependencyGraphResolvesAndExpands (Swift). */
    {
        ares_result r;
        size_t i;
        int has_mui = 0, has_020 = 0, has_68k = 0;
        long mui_pos = -1, igame_pos = -1;
        memset(&r, 0, sizeof r);
        ares_resolve(&idx, "igame", "68020", &r);
        CHECK(r.missing_count == 0, "igame deps all resolve on 68020");
        for (i = 0; i < r.count; i++) {
            if (strcmp(r.ordered[i]->id, "mui38") == 0) { has_mui = 1; mui_pos = (long)i; }
            if (strcmp(r.ordered[i]->id, "texteditormcc") == 0) has_020 = 1;
            if (strcmp(r.ordered[i]->id, "texteditormcc68k") == 0) has_68k = 1;
            if (strcmp(r.ordered[i]->id, "igame") == 0) igame_pos = (long)i;
        }
        CHECK(has_mui, "mui38 pulled in");
        CHECK(has_020 && !has_68k, "020 MCC variant on 68020");
        CHECK(mui_pos >= 0 && igame_pos > mui_pos, "deps precede dependent");

        memset(&r, 0, sizeof r);
        ares_resolve(&idx, "igame", "68000", &r);
        has_020 = has_68k = 0;
        for (i = 0; i < r.count; i++) {
            if (strcmp(r.ordered[i]->id, "texteditormcc") == 0) has_020 = 1;
            if (strcmp(r.ordered[i]->id, "texteditormcc68k") == 0) has_68k = 1;
        }
        CHECK(has_68k && !has_020, "68k MCC variant on stock 68000");

        /* ScummVM game pulls the engine. */
        memset(&r, 0, sizeof r);
        ares_resolve(&idx, "bass", "68060", &r);
        {
            long scumm_pos = -1, bass_pos = -1;
            for (i = 0; i < r.count; i++) {
                if (strcmp(r.ordered[i]->id, "scummvm") == 0) scumm_pos = (long)i;
                if (strcmp(r.ordered[i]->id, "bass") == 0) bass_pos = (long)i;
            }
            CHECK(scumm_pos >= 0 && bass_pos > scumm_pos, "bass pulls scummvm first");
        }

        /* Unknown selection is reported, not dropped. */
        memset(&r, 0, sizeof r);
        ares_resolve(&idx, "ghost", NULL, &r);
        CHECK(r.missing_count == 1 && strcmp(r.missing[0], "ghost") == 0, "unknown reported");
    }
    aidx_free(&idx);
}

/* Find the packages[] entry object with the given id in a parsed index tree. */
static const aj_node *find_entry_obj(const aj_node *root, const char *id)
{
    const aj_node *pkgs = ajson_get(root, "packages");
    const aj_node *c;
    if (!pkgs || pkgs->type != AJ_ARR) return 0;
    for (c = pkgs->child; c; c = c->next)
        if (c->type == AJ_OBJ && strcmp(ajson_get_str(c, "id", ""), id) == 0) return c;
    return 0;
}

static void test_recipe(void)
{
    /* Parse Roadie's recipe from the REAL index: 3 copyGlob + stripJunk +
     * mergeNested + several setExec — mirror of testCSVToRecipeConversion. */
    char *json = read_file("tests/fixtures/packages.json");
    aj_node *root;
    const aj_node *roadie, *wbdock;
    arecipe r;
    size_t i, copy = 0, setexec = 0, strip = 0, merge = 0;
    CHECK(json != NULL, "fixture readable (recipe)");
    if (!json) return;
    root = ajson_parse(json);
    free(json);
    CHECK(root != NULL, "index tree parses");
    if (!root) return;

    roadie = find_entry_obj(root, "roadie");
    CHECK(roadie != NULL, "roadie entry found");
    CHECK(arecipe_parse(roadie, &r) == 0, "roadie recipe parses");
    CHECK(!r.has_unknown, "roadie recipe has only known (Tier-A) ops");
    for (i = 0; i < r.op_count; i++) {
        switch (r.ops[i].type) {
        case AROP_COPY_GLOB: copy++; break;
        case AROP_SET_EXEC: setexec++; break;
        case AROP_STRIP_JUNK: strip++; break;
        case AROP_MERGE_NESTED: merge++; break;
        default: break;
        }
    }
    CHECK(copy == 3, "roadie: 3 copyGlob ops");
    CHECK(strip == 1 && merge == 1, "roadie: stripJunk + mergeNested");
    CHECK(setexec >= 8, "roadie: the standard setExec scopes");
    /* Programs scope is depth 2 (mirror of the Swift converter). */
    for (i = 0; i < r.op_count; i++)
        if (r.ops[i].type == AROP_SET_EXEC && strcmp(r.ops[i].scope, "Programs") == 0)
            CHECK(r.ops[i].depth == 2, "Programs setExec depth 2");

    /* wbdock is build-only (host-builtin) → no recipe. */
    wbdock = find_entry_obj(root, "wbdock");
    CHECK(wbdock != NULL, "wbdock entry found");
    CHECK(arecipe_parse(wbdock, &r) != 0, "wbdock (build-only) has no recipe");

    ajson_free(root);
}

static void test_arun(void)
{
    /* Glob matching: FNM_CASEFOLD, '*' spans '/'. */
    CHECK(arun_glob_match("Roadie*", "Roadie.info"), "Roadie* matches Roadie.info");
    CHECK(arun_glob_match("Roadie/*", "Roadie/Roadie"), "Roadie/* matches Roadie/Roadie");
    CHECK(arun_glob_match("roadie*", "ROADIE"), "casefold");
    CHECK(arun_glob_match("a/*/c", "a/b/c"), "single star spans one seg");
    CHECK(arun_glob_match("a*c", "a/b/c"), "star spans '/'");
    CHECK(!arun_glob_match("Roadie", "Roadie.info"), "no partial without star");

    /* Plan a synthetic install: one wildcard copy into Programs/App, one
     * exact copy with rename, junk excluded, plus setExec + scriptInject. */
    {
        arecipe r;
        arun_plan plan;
        const char *entries[] = {
            "App", "App/App", "App/App.info", "App/.DS_Store",
            "App/data", "App/data/x.dat", "Icon.info"
        };
        const int is_dir[] = { 1, 0, 0, 0, 1, 0, 0 };
        size_t i, execs = 0, scripts = 0;
        int saw_bin = 0, saw_renamed = 0, saw_junk = 0;

        memset(&r, 0, sizeof r);
        r.op_count = 4;
        r.ops[0].type = AROP_COPY_GLOB;
        strcpy(r.ops[0].src, "App/*"); strcpy(r.ops[0].dest, "Programs/App");
        r.ops[1].type = AROP_COPY_GLOB;   /* exact + rename */
        strcpy(r.ops[1].src, "Icon.info"); strcpy(r.ops[1].dest, "Programs");
        strcpy(r.ops[1].rename, "App.info");
        r.ops[2].type = AROP_SET_EXEC; strcpy(r.ops[2].scope, "Programs"); r.ops[2].depth = 2;
        r.ops[3].type = AROP_SCRIPT_INJECT;
        strcpy(r.ops[3].target, "S:User-Startup");
        strcpy(r.ops[3].overlay, "User-Startup_App"); strcpy(r.ops[3].marker, "App");

        CHECK(arun_plan_build(&r, entries, is_dir, 7, &plan) == 0, "plan builds");
        for (i = 0; i < plan.count; i++) {
            switch (plan.ops[i].kind) {
            case ARUN_COPY:
                if (strcmp(plan.ops[i].dest, "Programs/App/App") == 0) saw_bin = 1;
                if (strcmp(plan.ops[i].dest, "Programs/App.info") == 0) saw_renamed = 1;
                if (strstr(plan.ops[i].src, ".DS_Store")) saw_junk = 1;
                break;
            case ARUN_SET_EXEC: execs++; break;
            case ARUN_SCRIPT: scripts++;
                CHECK(strcmp(plan.ops[i].marker, "App") == 0, "script marker");
                break;
            default: break;
            }
        }
        /* App-slash-star (depth 2) matches App/App + App/App.info, skips the
         * junk .DS_Store; App/data is a dir (skipped); App/data/x.dat is depth 3. */
        CHECK(saw_bin, "App/App copied to Programs/App/App");
        CHECK(saw_renamed, "Icon.info renamed to Programs/App.info (exact + rename)");
        CHECK(!saw_junk, ".DS_Store excluded");
        CHECK(execs == 1 && scripts == 1, "setExec + scriptInject planned");
    }

    /* A recipe with an unknown op is refused (not partially run). */
    {
        arecipe r;
        arun_plan plan;
        memset(&r, 0, sizeof r);
        r.has_unknown = 1;
        CHECK(arun_plan_build(&r, 0, 0, 0, &plan) != 0, "unknown-op recipe refused");
    }
}

/* preScript/postScript: inline lines join with \n, plan-build ignores them
 * (they run around the plan), and the capability keeps the recipe runnable. */
static void test_prepost_script(void)
{
    const char *json =
        "{\"id\":\"x\",\"recipe\":{\"recipeSchema\":1,\"ops\":["
        "{\"op\":\"preScript\",\"lines\":[\"Assign FOO: SYS:Foo\",\"Echo pre\"]},"
        "{\"op\":\"copyGlob\",\"src\":\"*\",\"dest\":\"Programs/X\",\"recursive\":true},"
        "{\"op\":\"postScript\",\"lines\":[\"Echo done\"]}]}}";
    aj_node *root = ajson_parse(json);
    arecipe r;
    arun_plan *plan = malloc(sizeof(arun_plan));
    const char *entries[1] = { "file1" };
    int is_dir[1] = { 0 };
    CHECK(root != NULL, "prepost: json parses");
    CHECK(arecipe_parse(root, &r) == 0, "prepost: recipe parses");
    CHECK(!r.has_unknown, "prepost: ops are known");
    CHECK(r.op_count == 3, "prepost: 3 ops");
    CHECK(r.ops[0].type == AROP_PRE_SCRIPT, "prepost: op0 is preScript");
    CHECK(strcmp(r.ops[0].script, "Assign FOO: SYS:Foo\nEcho pre") == 0,
          "prepost: lines joined with newline");
    CHECK(r.ops[2].type == AROP_POST_SCRIPT, "prepost: op2 is postScript");
    CHECK(plan != NULL, "prepost: plan alloc");
    CHECK(arun_plan_build(&r, entries, is_dir, 1, plan) == 0,
          "prepost: plan builds (scripts are not plan ops)");
    CHECK(plan->count == 1, "prepost: only the copy is planned");
    free(plan);
    ajson_free(root);
}

int main(void)
{
    test_sha256();
    test_aver();
    test_receipts();
    test_ajson();
    test_real_index();
    test_recipe();
    test_prepost_script();
    test_arun();
    if (failures == 0) {
        printf("amipkg core: ALL TESTS PASSED\n");
        return 0;
    }
    printf("amipkg core: %d FAILURE(S)\n", failures);
    return 1;
}
