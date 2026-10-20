/*
 * dxcts - runs HLK graphics conformance tests and reports what happened.
 *
 * Tests are spawned rather than hosted: the framework binaries are executables, and
 * process isolation keeps a crashing or wedged test from taking the run with it.
 *
 * The report distinguishes a case that ran and failed from one that never ran.  That
 * distinction cannot be recovered from a log file, where the framework collapses both
 * into "Fail"; it comes from the WTTLog proxy's event stream, which sees the actual
 * StartTest/EndTest pairs.
 */

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../common/backend.h"
#include "../common/json.h"
#include "../common/knob.h"

#define MAX_BINARIES 256
/* Per binary.  WGF11Shader5x alone runs 339,622 cases; at 2^18 it was silently cut to
 * 262,144 and every later group lost its records. */
#define MAX_CASES (1 << 22)
/*
 * Sized from measurement, not guessed.  Every hang this suite used to have is fixed,
 * so a timeout now means the host is pathologically slow at something - which is worth
 * detecting, but only once the test has been given a fair chance to finish.  At 900s
 * both arms cut off tests that were still making progress:
 *
 *   Clipping\Triangle\ZClip   24256 cases; WARP does it in 354s (68/s), D3DMetal
 *                              manages 4/s and needs ~100 minutes
 *   RenderTargets              WARP itself needed more than 928s
 *
 * Two hours per group covers those with room.  --slow is what notices a group growing
 * long, so nothing has to wait for this to expire to be seen.
 */
#define DEFAULT_TIMEOUT_S 7200
/* A group that has not finished a case in this long, with its process still
 * running, is wedged rather than slow: the slowest group measured here needs
 * minutes per case at worst, and a wedge never finishes another one. */
#define DEFAULT_STALL_S 600

/* Opened rather than stat'ed: NTFS updates a directory entry lazily while another
 * process holds the file open, so the size a stat reports can sit at zero for a file
 * the test is writing to, and the watchdog would call a live group wedged. */
static long long event_file_size(const char *path)
{
    LARGE_INTEGER size;
    HANDLE handle;
    long long result = -1;

    if (!path)
        return -1;
    handle = CreateFileA(path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileSizeEx(handle, &size))
        result = size.QuadPart;
    CloseHandle(handle);
    return result;
}

/*
 * A group taking longer than this is reported but not failed.  The timeout is
 * deliberately conservative - a test that is merely slow, which WARP has plenty of,
 * must never be cut short and reported as a hang - so on its own it would let a group
 * quietly grow from seconds to minutes with nothing to notice it.  This is what
 * notices: it names the group and its time, and the report carries slow_groups, so a
 * new host-side stall shows up as a number rather than as a timeout nobody sees until
 * the budget is finally exceeded.
 */
#define DEFAULT_SLOW_S 60

/* CWTTLogger result codes, as observed in the event stream. */
enum
{
    WTT_PASS = 1,
    WTT_FAIL = 2,
    WTT_BLOCKED = 3,
    WTT_WARN = 4,
    WTT_SKIP = 5,
};

typedef struct
{
    char binary[128];
    char harness[16];
    char api[24];
    char skip[256];
    int software_adapter;
    int dwm_output;
    /* Data the binary reads from a fixed path outside the staging tree, which the
     * HLK's job deploys there before running it.  Copied from the staging tree
     * (deploy_from, relative) to deploy_to (environment variables expanded). */
    char deploy_from[128];
    char deploy_to[128];
    /* Seconds this binary's groups actually need.  A measured cost, not a way to hide
     * a hang: a group that makes no progress is still a timeout however long the
     * bound.  An explicit --timeout overrides it; the default does not, or the value
     * would only ever matter when it happened to exceed the default. */
    long long timeout_s;
} catalog_entry;

#define MAX_EXCLUSIONS 512

typedef struct exclusion
{
    char binary[128];
    char harness[16];
    char group[256];
    char cases[4096];
    char reason[2048];
    int line;
    /* The binary was selected this run, and the entry did or did not apply to
     * anything in it: an entry whose binary ran without it matching is stale. */
    int binary_seen;
    int matched;
} exclusion;

/* What a run recorded about the exclusions it applied, so its counts explain
 * themselves. */
typedef struct
{
    char group[256];
    const exclusion *entry;
} applied_exclusion;

typedef struct
{
    /* Interned: a group's cases all carry its name, and a copy per record made a
     * whole-run report of a million cases cost gigabytes. */
    const char *name;
    /* The process that logged it.  A test may spawn helpers, and they inherit
     * DXCTS_EVENTS and the proxy, so two processes can have a case of the same name
     * open at once; pairing on the name alone lets one close the other's. */
    unsigned long pid;
    int executed;
    int have_result;
    /* A TAEF setup/cleanup fixture rather than a test method.  It is kept, because a
     * failed fixture is why the methods it guards did not run, but it is not counted
     * as one of them. */
    int fixture;
    unsigned long result;
} case_record;

#define MAX_REASONS 8

typedef struct
{
    char text[160];
    int count;
} failure_reason;

typedef struct group_result
{
    char name[256];
    double seconds;
    int cases;
    int timed_out;
    /* the host faulted somewhere it cannot recover from and the process stopped
     * making progress; distinct from timed_out, which is only "took too long" */
    int wedged;
    DWORD exit_code;
    int altered;            /* draws a workaround suppressed; see the shim */
    int host_errors;        /* host validation failures the run survived */
    int slow;               /* took longer than the reporting threshold */
    /* WGF case numbers the process died in; the group was resumed after each */
    char crashed[512];
    int crash_count;
    /* why this group's cases failed, read back from its log so the report can be
     * analysed without it */
    failure_reason reasons[MAX_REASONS];
    int reason_count;
} group_result;

typedef struct
{
    catalog_entry *entry;
    DWORD exit_code;
    int timed_out;
    int wedged;
    double seconds;
    case_record *cases;
    int case_count;
    int case_capacity;
    int truncated;
    int altered;
    int host_errors;
    int suspect_pass;       /* passes recorded while the host reported a failure */
    int slow;               /* whole-binary runs have no group to carry this */
    int selectors_ignored;  /* SELECTOR_*: asked for, and this harness cannot */
    char crashed[512];      /* WGF case numbers a crash was resumed past */
    int crash_count;
    /* What TAEF itself reported in its Summary line, so a binary whose console
     * output and case records disagree is visible rather than silently believed. */
    int have_harness_total;
    long long harness_total;
    /* Why the cases failed when the binary ran as one process and so has no
     * group to carry it; the same summary a group gets. */
    failure_reason reasons[MAX_REASONS];
    int reason_count;
    /* Per-group outcomes, so "this binary timed out" says which group did. */
    group_result *groups;
    int group_count;
    int group_capacity;
    /* The entry that kept the whole binary out of the run, or what it kept out of
     * the binary: whole groups, and cases within the groups that did run. */
    const struct exclusion *excluded;
    applied_exclusion *applied;
    int applied_count;
    int applied_capacity;
} run_record;

static catalog_entry g_catalog[MAX_BINARIES];
static int g_catalog_count;

/*
 * Where progress goes, or NULL when it must go nowhere.
 *
 * It cannot share a stream with either of the other two writers.  stderr belongs to the
 * host, whose validation failures are counted out of it, and stdout is where the report
 * goes when no --out was given - a progress line there is the first byte of what should
 * be a JSON document.  So with a file to write the report to, progress goes to stdout;
 * without one, there is nowhere left and it is suppressed.
 */
static FILE *g_progress;

/* --- options --- */

typedef struct
{
    const char *catalog;
    const char *out;
    const char *api;
    const char *harness;
    const char *binary;
    const char *group;
    const char *sample;
    const char *seed;
    const char *adapter_type;
    const char *src;
    const char *graphics_finish_ms;
    const char *query_timeout_ms;
    unsigned timeout_s;
    int timeout_given;
    unsigned slow_s;
    unsigned stall_s;
    int per_group;
    int include_skipped;
    int no_patches;
    int d3d11on12;
    const char *backend;
    const char *exclusion;
} options;

static void usage(void)
{
    fputs(
        "usage: dxcts <list|run|probe> [options]\n"
        "\n"
        "  --catalog PATH     suite catalog (default: suites/graphics.jsonl beside dxcts)\n"
        "  --api NAME         only this API (d3d11, d3d12, dxgi, ...)\n"
        "  --harness NAME     only this harness family (wgf, wgf9, taef, gtest)\n"
        "  --binary NAME      only this test binary\n"
        "  --group PATH       group or method selector passed to the test\n"
        "  --sample PERCENT   run a deterministic sample; a percentage, rounded up\n"
        "                     to at least one case per group, so 0.002 is one case\n"
        "                     per group and exercises every group in the suite\n"
        "  --seed N           sample seed (default 1)\n"
        "  --adapter-type T   all|fullhw|renderonlyhw|fullsw|renderonlysw (default all)\n"
        "  --src NAME         framework device source: REF, HW, SW, NULL or WARP\n"
        "  --11on12           run the D3D11 WGF binaries on D3D11On12, so the D3D12\n"
        "                     implementation is what is under test; this is how the\n"
        "                     kit's own D3D12 job list runs them\n"
        "  --timeout SECONDS  limit per spawned process - per group under\n"
        "                     --per-group, per binary otherwise (default 7200);\n"
        "                     given explicitly it also overrides the catalog's\n"
        "                     per-binary value\n"
        "  --stall SECONDS    give up on a group whose last case finished this long\n"
        "                     ago while its process is still running (default 600, 0\n"
        "                     disables); a host fault Wine cannot raise as an exception\n"
        "                     leaves the process spinning with no further cases, and\n"
        "                     that is reported as wedged rather than waiting out the\n"
        "                     whole timeout\n"
        "  --slow SECONDS     report a group taking longer than this (default 60);\n"
        "                     it is not failed, only named, so a new host-side stall\n"
        "                     is visible long before it reaches the timeout\n"
        "  --finish-ms MS     wait for pending graphics work (default 1000; the\n"
        "                     framework default of 10000 is spent in full on every\n"
        "                     operation a host implementation never signals)\n"
        "  --query-ms MS      query completion wait (default 1000)\n"
        "  --per-group        run each test group as its own process, so a group that\n"
        "                     wedges the host costs only its own timeout instead of\n"
        "                     everything after it in that binary\n"
        "  --out PATH         write the JSON report here (default: stdout, in which\n"
        "                     case progress is suppressed so the report is the only\n"
        "                     thing written there)\n"
        "  --include-skipped  also run binaries the catalog marks unrunnable\n"
        "  --backend NAME     apply the compatibility layer for this host D3D\n"
        "                     implementation (dxcts_backend_NAME.dll beside the shims;\n"
        "                     d3dmetal); off unless named, and recorded in the report\n"
        "  --exclusion LIST   leave out what LIST names: a path, or the name of a list\n"
        "                     in suites\\exclusions beside dxcts (warp, d3dmetal, ...);\n"
        "                     every entry carries its reason, and the report lists\n"
        "                     what was left out and why\n"
        "  --no-patches       disable every shim workaround, to measure what one is\n"
        "                     worth; individual ones have their own DXCTS_* knobs\n",
        stderr);
}

/* --- catalog --- */

static int load_catalog(const char *path)
{
    char line[2048];
    FILE *fh = fopen(path, "r");
    int lineno = 0;

    if (!fh)
    {
        fprintf(stderr, "dxcts: cannot open catalog %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), fh))
    {
        json_line parsed;
        char err[128];
        catalog_entry *e;

        lineno++;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
            continue;
        if (g_catalog_count >= MAX_BINARIES)
        {
            fprintf(stderr, "dxcts: catalog holds more than %d binaries\n",
                    MAX_BINARIES);
            break;
        }
        if (!json_parse_line(line, &parsed, err, sizeof(err)))
        {
            fprintf(stderr, "dxcts: %s:%d: %s\n", path, lineno, err);
            fclose(fh);
            return 0;
        }
        e = &g_catalog[g_catalog_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->binary, sizeof(e->binary), "%s", json_get(&parsed, "binary", ""));
        snprintf(e->harness, sizeof(e->harness), "%s", json_get(&parsed, "harness", ""));
        snprintf(e->api, sizeof(e->api), "%s", json_get(&parsed, "api", ""));
        snprintf(e->skip, sizeof(e->skip), "%s", json_get(&parsed, "skip", ""));
        e->software_adapter = json_get_bool(&parsed, "software_adapter", 0);
        snprintf(e->deploy_from, sizeof(e->deploy_from), "%s",
                 json_get(&parsed, "deploy_from", ""));
        snprintf(e->deploy_to, sizeof(e->deploy_to), "%s",
                 json_get(&parsed, "deploy_to", ""));
        e->dwm_output = json_get_bool(&parsed, "dwm_output", 0);
        e->timeout_s = json_get_int(&parsed, "timeout_s", 0);
    }
    fclose(fh);
    return g_catalog_count > 0;
}

/*
 * A Wine launcher rewrites an argument that looks like a file name into a Windows path,
 * so --binary WGF11Blend.exe arrives as C:\dxcts\WGF11Blend.exe.  Matching on the last
 * path component accepts both, and a path the user typed themselves too.
 */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '\\');
    const char *fwd = strrchr(path, '/');

    if (fwd > slash)
        slash = fwd;
    return slash ? slash + 1 : path;
}

/* --- exclusions --- */

/*
 * Tests known to wedge, crash or otherwise break a run on one implementation, kept out
 * of it so that one command runs everything else and gives the same answer twice.  One
 * file per implementation, one flat JSON object per line like the catalog:
 *
 *   {"binary":"WGF11Foo.exe","group":"Foo\\Bar","cases":"12-15,40","reason":"..."}
 *
 * binary may be replaced or narrowed by harness, naming a whole framework family that
 * cannot run on the implementation.  group absent excludes the whole binary; cases absent, the whole group.  binary and
 * group take * and ? wildcards, and a WGF group also covers everything under it, as
 * the framework's own "!Group" does.  cases is the WGF case numbers the framework
 * prints, or for TAEF and gtest the method or test names within the group, comma
 * separated.  Every line must say why: a line without a reason is refused, so the list
 * cannot grow entries nobody can justify removing.
 *
 * Fixing the harness is preferred to adding a line here; a line is what is left when
 * the fault is the implementation's and cannot be survived.
 */

static exclusion g_exclusions[MAX_EXCLUSIONS];
static int g_exclusion_count;
static char g_exclusion_path[MAX_PATH];

static const char *const k_exclusion_keys[] = {
    "binary", "harness", "group", "cases", "reason", "evidence",
};

/* Case-insensitive, * and ? only: names on both sides are Windows file names and
 * framework paths, neither of which is case-sensitive. */
static int wildcard_match(const char *pattern, const char *text)
{
    const char *star = NULL, *resume = NULL;

    while (*text)
    {
        if (*pattern == '*')
        {
            star = pattern++;
            resume = text;
        }
        else if (*pattern == '?'
                 || tolower((unsigned char)*pattern) == tolower((unsigned char)*text))
        {
            pattern++;
            text++;
        }
        else if (star)
        {
            pattern = star + 1;
            text = ++resume;
        }
        else
            return 0;
    }
    while (*pattern == '*')
        pattern++;
    return !*pattern;
}

/* Whether an entry names this binary: by name, by harness family, or both. */
static int exclusion_names_binary(const struct exclusion *x, const catalog_entry *e)
{
    return wildcard_match(x->binary, e->binary)
        && (!x->harness[0] || !strcmp(x->harness, e->harness));
}

static int has_wildcard(const char *s)
{
    return strpbrk(s, "*?") != NULL;
}

/*
 * A bare name ("warp") is a list staged beside dxcts in suites\exclusions; anything with
 * a separator or an extension is a path.
 */
static void resolve_exclusion_path(const char *arg, char *out, size_t cap)
{
    char exe[MAX_PATH];
    char *slash;

    if (strpbrk(arg, "\\/.:") || !GetModuleFileNameA(NULL, exe, MAX_PATH))
    {
        snprintf(out, cap, "%s", arg);
        return;
    }
    slash = strrchr(exe, '\\');
    if (slash)
        *slash = 0;
    snprintf(out, cap, "%s\\suites\\exclusions\\%s.jsonl", exe, arg);
}

static int load_exclusions(const char *arg)
{
    char line[16384];
    FILE *fh;
    int lineno = 0, ok = 1;

    resolve_exclusion_path(arg, g_exclusion_path, sizeof(g_exclusion_path));
    fh = fopen(g_exclusion_path, "r");
    if (!fh)
    {
        fprintf(stderr, "dxcts: cannot open exclusion list %s\n", g_exclusion_path);
        return 0;
    }
    while (fgets(line, sizeof(line), fh))
    {
        json_line parsed;
        char err[128];
        exclusion *x;
        int k, j;

        lineno++;
        /* fgets would hand the rest of a longer line over as the next one */
        if (!strchr(line, '\n') && !feof(fh))
        {
            fprintf(stderr, "dxcts: %s:%d: line longer than %d bytes\n",
                    g_exclusion_path, lineno, (int)sizeof(line) - 2);
            ok = 0;
            break;
        }
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
            continue;
        if (!json_parse_line(line, &parsed, err, sizeof(err)))
        {
            fprintf(stderr, "dxcts: %s:%d: %s\n", g_exclusion_path, lineno, err);
            ok = 0;
            continue;
        }
        /* An unknown key is a misspelt one, and a misspelt "group" would silently
         * widen the entry to the whole binary. */
        for (k = 0; k < parsed.count; k++)
        {
            for (j = 0; j < (int)(sizeof(k_exclusion_keys) / sizeof(k_exclusion_keys[0])); j++)
                if (!strcmp(parsed.fields[k].key, k_exclusion_keys[j]))
                    break;
            if (j == (int)(sizeof(k_exclusion_keys) / sizeof(k_exclusion_keys[0])))
            {
                fprintf(stderr, "dxcts: %s:%d: unknown key \"%s\"\n", g_exclusion_path,
                        lineno, parsed.fields[k].key);
                ok = 0;
            }
        }
        if (g_exclusion_count >= MAX_EXCLUSIONS)
        {
            fprintf(stderr, "dxcts: %s holds more than %d exclusions\n",
                    g_exclusion_path, MAX_EXCLUSIONS);
            ok = 0;
            break;
        }
        x = &g_exclusions[g_exclusion_count];
        memset(x, 0, sizeof(*x));
        x->line = lineno;
        snprintf(x->binary, sizeof(x->binary), "%s", json_get(&parsed, "binary", "*"));
        snprintf(x->harness, sizeof(x->harness), "%s", json_get(&parsed, "harness", ""));
        snprintf(x->group, sizeof(x->group), "%s", json_get(&parsed, "group", ""));
        snprintf(x->cases, sizeof(x->cases), "%s", json_get(&parsed, "cases", ""));
        snprintf(x->reason, sizeof(x->reason), "%s", json_get(&parsed, "reason", ""));
        if (!json_get(&parsed, "binary", NULL) && !x->harness[0])
        {
            fprintf(stderr, "dxcts: %s:%d: an exclusion needs a binary or a harness\n",
                    g_exclusion_path, lineno);
            ok = 0;
        }
        if (!x->reason[0])
        {
            fprintf(stderr, "dxcts: %s:%d: an exclusion needs a reason\n",
                    g_exclusion_path, lineno);
            ok = 0;
        }
        if (x->cases[0] && !x->group[0])
        {
            fprintf(stderr, "dxcts: %s:%d: cases needs a group\n", g_exclusion_path,
                    lineno);
            ok = 0;
        }
        g_exclusion_count++;
    }
    fclose(fh);
    return ok;
}

/* The entry excluding this whole binary, if one does. */
static exclusion *binary_exclusion(const catalog_entry *e)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (!g_exclusions[i].group[0] && exclusion_names_binary(&g_exclusions[i], e))
            return &g_exclusions[i];
    return NULL;
}

static int exclusion_covers_group(const exclusion *x, const catalog_entry *e,
                                  const char *group)
{
    size_t len = strlen(x->group);

    if (!x->group[0] || !exclusion_names_binary(x, e))
        return 0;
    if (wildcard_match(x->group, group))
        return 1;
    /* "!Group" in the WGF framework takes the subtree under it, so the entry does too */
    return !strcmp(e->harness, "wgf") && !_strnicmp(x->group, group, len)
        && group[len] == '\\';
}

/* The entry excluding this whole group, if one does. */
static exclusion *group_exclusion(const catalog_entry *e, const char *group)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (!g_exclusions[i].cases[0] && exclusion_covers_group(&g_exclusions[i], e, group))
            return &g_exclusions[i];
    return NULL;
}

static void mark_binary_seen(const catalog_entry *e)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (exclusion_names_binary(&g_exclusions[i], e))
            g_exclusions[i].binary_seen = 1;
}

static int selected(const catalog_entry *e, const options *o)
{
    if (o->api && strcmp(o->api, e->api) != 0)
        return 0;
    if (o->harness && strcmp(o->harness, e->harness) != 0)
        return 0;
    if (o->binary && _stricmp(base_name(o->binary), e->binary) != 0)
        return 0;
    if (e->skip[0] && !o->include_skipped)
        return 0;
    return 1;
}

/* --- event stream --- */

/*
 * A group name is not unique: the framework runs every parameter combination in a
 * group under that group's name, so WGF11Clipping's six groups produce 155 cases.
 * Each start/end pair is therefore its own record, and an end completes the most
 * recent open record with a matching name from the same process.
 */
/* Case names repeat (a WGF group logs every case under its own name), so each distinct
 * one is stored once for the life of the run. */
static const char *intern(const char *name)
{
    static char **table;
    static size_t capacity, used;
    size_t h = 5381, i;

    if (used * 2 >= capacity)
    {
        size_t want = capacity ? capacity * 2 : 4096, j;
        char **grown = calloc(want, sizeof(*grown));

        if (!grown)
            return NULL;
        for (j = 0; j < capacity; j++)
        {
            size_t k = 5381;
            const char *t;

            if (!table[j])
                continue;
            for (t = table[j]; *t; t++)
                k = k * 33 + (unsigned char)*t;
            for (k &= want - 1; grown[k]; k = (k + 1) & (want - 1))
                ;
            grown[k] = table[j];
        }
        free(table);
        table = grown;
        capacity = want;
    }
    for (i = 0; name[i]; i++)
        h = h * 33 + (unsigned char)name[i];
    for (h &= capacity - 1; table[h]; h = (h + 1) & (capacity - 1))
        if (!strcmp(table[h], name))
            return table[h];
    table[h] = _strdup(name);
    if (table[h])
        used++;
    return table[h];
}

static case_record *open_case(run_record *r, const char *name, unsigned long pid)
{
    case_record *c;

    if (r->case_count >= r->case_capacity)
    {
        int want = r->case_capacity ? r->case_capacity * 2 : 256;
        case_record *grown;

        if (want > MAX_CASES)
            want = MAX_CASES;
        if (want <= r->case_count)
        {
            r->truncated = 1;
            return NULL;
        }
        grown = realloc(r->cases, (size_t)want * sizeof(*grown));
        if (!grown)
        {
            r->truncated = 1;
            return NULL;
        }
        r->cases = grown;
        r->case_capacity = want;
    }
    c = &r->cases[r->case_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->name = intern(name);
    if (!c->name)
    {
        r->case_count--;
        r->truncated = 1;
        return NULL;
    }
    return c;
}

static case_record *close_case(run_record *r, const char *name, unsigned long pid)
{
    int i;
    for (i = r->case_count - 1; i >= 0; i--)
        if (!r->cases[i].have_result && r->cases[i].pid == pid
            && strcmp(r->cases[i].name, name) == 0)
            return &r->cases[i];
    return NULL;
}

static void read_events(run_record *r, const char *path)
{
    char line[2048];
    FILE *fh = fopen(path, "r");

    if (!fh)
        return;
    while (fgets(line, sizeof(line), fh))
    {
        json_line parsed;
        char err[128];
        const char *event, *name;
        unsigned long pid;
        case_record *c;

        if (!json_parse_line(line, &parsed, err, sizeof(err)))
            continue;
        event = json_get(&parsed, "event", "");
        name = json_get(&parsed, "name", "");
        pid = (unsigned long)json_get_int(&parsed, "pid", 0);
        if (strcmp(event, "start") == 0)
        {
            c = open_case(r, name, pid);
            if (!c)
                break;
            c->executed = 1;
        }
        else if (strcmp(event, "workaround") == 0)
        {
            /* A shim changed what the test saw.  Carried into the report so the
             * group's verdicts are never mistaken for unaltered ones. */
            r->altered += json_get_int(&parsed, "count", 0);
        }
        else if (strcmp(event, "end") == 0)
        {
            c = close_case(r, name, pid);
            if (!c)
            {
                /* An end with no open start: record it so the case is not lost. */
                c = open_case(r, name, pid);
                if (!c)
                    break;
            }
            c->have_result = 1;
            c->result = (unsigned long)json_get_int(&parsed, "result", 0);
        }
    }
    fclose(fh);
}

/*
 * TE.exe writes UTF-16 and the framework binaries write ASCII - and worse, the host
 * implementation logs ASCII into TE.exe's UTF-16 stdout, one line at a time.  An
 * odd-length insertion shifts every character after it by a byte, so a reader that
 * trusts the encoding loses the rest of the file: D3DConf_12_0_ResourceBarrierDepthStencil
 * ran three cases and was recorded as none because one unsupported-feature message
 * landed in the middle of the results.
 *
 * Everything matched here is ASCII in either encoding, so the NUL bytes are simply
 * dropped and the rest read as bytes.  That is immune to where the insertions land.
 */
static FILE *open_text(const char *path)
{
    return fopen(path, "rb");
}

static int read_text_line(FILE *fh, char *line, size_t cap)
{
    size_t len = 0;
    int any = 0;

    for (;;)
    {
        int ch = fgetc(fh);

        if (ch == EOF)
            break;
        any = 1;
        if (ch == 0 || ch == '\r')
            continue;
        if (ch == '\n')
        {
            line[len] = 0;
            return 1;
        }
        if (len + 1 < cap)
            line[len++] = (char)ch;
    }
    line[len] = 0;
    return any;
}

/*
 * TAEF results.
 *
 * Wex.Logger's WTT device binds only the flat WTTLogTrace* entry points, never
 * CWTTLogger::StartTest/EndTest, so the WTTLog tap sees no case boundaries for a
 * TAEF binary and every method would be reported as never run.  TE.exe's console
 * output does carry them - one "StartGroup:"/"EndGroup:" pair per method, verdict in
 * brackets - so that is what is read back.
 *
 * These come from one process's console output rather than the event stream, so they
 * share one synthetic pid and pair among themselves.
 */
#define TAEF_CONSOLE_PID 0

static void read_taef_console(run_record *r, const char *path)
{
    FILE *fh = open_text(path);
    char line[1024];

    if (!fh)
        return;
    while (read_text_line(fh, line, sizeof(line)))
    {
        if (!strncmp(line, "StartGroup: ", 12))
        {
            case_record *c = open_case(r, line + 12, TAEF_CONSOLE_PID);

            if (c)
            {
                c->executed = 1;
                /* Every method TAEF lists is <class>::<method>; anything without
                 * that is one of its fixtures. */
                c->fixture = strstr(line + 12, "::") == NULL;
            }
        }
        else if (!strncmp(line, "EndGroup: ", 10))
        {
            char *bracket = strrchr(line, '[');
            unsigned long result = WTT_FAIL;
            case_record *c;

            if (bracket && bracket > line + 10)
            {
                if (!strcmp(bracket, "[Passed]")) result = WTT_PASS;
                else if (!strcmp(bracket, "[Blocked]")) result = WTT_BLOCKED;
                else if (!strcmp(bracket, "[Skipped]")) result = WTT_SKIP;
                else if (!strcmp(bracket, "[Not Run]")) result = 0;
                bracket[-1] = 0;        /* the space before the verdict */
            }
            c = close_case(r, line + 10, TAEF_CONSOLE_PID);
            if (!c)
            {
                c = open_case(r, line + 10, TAEF_CONSOLE_PID);
                if (!c)
                    continue;
                c->executed = 1;
                c->fixture = strstr(line + 10, "::") == NULL;
            }
            /* "Not Run" is TAEF reporting a method it never entered, a failed setup
             * fixture being the usual cause.  That is exactly the distinction the
             * report exists to keep, so it stays unexecuted rather than gaining a
             * verdict it never earned. */
            if (result == 0)
            {
                c->executed = 0;
                continue;
            }
            c->have_result = 1;
            c->result = result;
        }
        else if (!strncmp(line, "Summary: Total=", 15))
        {
            r->harness_total = atoll(line + 15);
            r->have_harness_total = 1;
        }
    }
    fclose(fh);
}

/*
 * gtest results, read from the JSON the binary was asked to write.
 *
 * A gtest binary logs through neither CWTTLogger nor TAEF's console, so without this
 * the run records nothing for it at all.  The document is nested and pretty-printed,
 * one key per line: the root holds "testsuites", each suite holds "testsuite", and
 * each test in that array carries "name", "status" ("RUN" or "NOTRUN"), "result"
 * ("COMPLETED" or "SKIPPED"), "classname" and, when it failed, a "failures" array.
 * Depth is tracked by counting braces outside strings, so a brace inside a failure
 * message cannot end an object early.
 */
#define GTEST_PID 0

static const char *json_line_string(const char *line, const char *key, char *out,
                                    size_t cap)
{
    const char *p = strstr(line, key);
    size_t i = 0;

    if (!p)
        return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"')
        return NULL;
    for (p++; *p && *p != '"' && i + 1 < cap; p++)
    {
        if (*p == '\\' && p[1])
            p++;
        out[i++] = *p;
    }
    out[i] = 0;
    return out;
}

/*
 * gtest's XML report, one tag per line: <testcase name=".." status="run|notrun"
 * classname=".."> with a <failure> or <skipped> child.  Dawn's launcher writes this
 * into the path given for --gtest_output=json, so both formats are read.
 */
static const char *xml_attr(const char *line, const char *attr, char *out, size_t cap)
{
    const char *p = strstr(line, attr);
    size_t i = 0;

    if (!p)
        return NULL;
    p += strlen(attr);
    for (; *p && *p != '"' && i + 1 < cap; p++)
        out[i++] = *p;
    out[i] = 0;
    return out;
}

static void read_gtest_xml(run_record *r, FILE *fh)
{
    char line[4096], name[256] = "", cls[256] = "", status[32] = "";
    int in_test = 0, failed = 0, skipped = 0;

    while (fgets(line, sizeof(line), fh))
    {
        const char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "<testcase ", 10))
        {
            xml_attr(p, "name=\"", name, sizeof(name));
            xml_attr(p, "status=\"", status, sizeof(status));
            xml_attr(p, "classname=\"", cls, sizeof(cls));
            in_test = 1;
            failed = skipped = 0;
            if (!strstr(p, "/>"))
                continue;
        }
        else if (in_test && !strncmp(p, "<failure", 8))
        {
            failed = 1;
            continue;
        }
        else if (in_test && !strncmp(p, "<skipped", 8))
        {
            skipped = 1;
            continue;
        }
        else if (!in_test || strncmp(p, "</testcase>", 11))
            continue;
        {
            char full[512];
            case_record *c;

            snprintf(full, sizeof(full), "%s.%s", cls, name);
            c = open_case(r, full, GTEST_PID);
            if (!c)
                break;
            c->executed = strcmp(status, "run") == 0;
            if (c->executed)
            {
                c->have_result = 1;
                c->result = failed ? WTT_FAIL : skipped ? WTT_SKIP : WTT_PASS;
            }
            r->harness_total++;
            r->have_harness_total = 1;
            in_test = 0;
        }
    }
}

static void read_gtest_json(run_record *r, const char *path)
{
    FILE *fh = fopen(path, "rb");
    char line[4096], name[256] = "", cls[256] = "", status[32] = "", result[32] = "";
    int depth = 0, in_string = 0, failures = 0, in_test = 0;

    if (!fh)
        return;
    {
        int ch = fgetc(fh);

        while (ch == 0xEF || ch == 0xBB || ch == 0xBF || ch == ' ' || ch == '\r'
               || ch == '\n')
            ch = fgetc(fh);
        if (ch == '<')
        {
            fseek(fh, 0, SEEK_SET);
            read_gtest_xml(r, fh);
            fclose(fh);
            return;
        }
        fseek(fh, 0, SEEK_SET);
    }
    while (fgets(line, sizeof(line), fh))
    {
        const char *p;
        int before = depth;

        if (depth == 3)
        {
            in_test = 1;
            if (json_line_string(line, "\"name\"", name, sizeof(name))) {}
            else if (json_line_string(line, "\"status\"", status, sizeof(status))) {}
            else if (json_line_string(line, "\"result\"", result, sizeof(result))) {}
            else if (json_line_string(line, "\"classname\"", cls, sizeof(cls))) {}
            else if (strstr(line, "\"failures\""))
                failures = 1;
        }
        for (p = line; *p; p++)
        {
            if (in_string)
            {
                if (*p == '\\' && p[1])
                    p++;
                else if (*p == '"')
                    in_string = 0;
            }
            else if (*p == '"')
                in_string = 1;
            else if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
        }
        if (before == 3 && depth == 2 && in_test && name[0])
        {
            char full[512];
            case_record *c;

            snprintf(full, sizeof(full), "%s.%s", cls, name);
            c = open_case(r, full, GTEST_PID);
            if (!c)
                break;
            c->executed = strcmp(status, "RUN") == 0;
            if (c->executed)
            {
                c->have_result = 1;
                c->result = failures ? WTT_FAIL
                          : !strcmp(result, "SKIPPED") ? WTT_SKIP : WTT_PASS;
            }
            r->harness_total++;
            r->have_harness_total = 1;
            name[0] = cls[0] = status[0] = result[0] = 0;
            failures = 0;
            in_test = 0;
        }
    }
    fclose(fh);
}

/*
 * The verdict a case is reported under.  Kept separate from the CWTTLogger code so
 * "never ran" and "ran but produced no end record" are verdicts of their own rather
 * than being folded into a failure - which is the distinction the report exists for.
 */
typedef enum
{
    VERDICT_NOT_RUN, VERDICT_INCOMPLETE, VERDICT_PASS, VERDICT_FAIL,
    VERDICT_BLOCKED, VERDICT_WARN, VERDICT_SKIP, VERDICT_UNKNOWN,
    VERDICT_COUNT
} verdict;

static const char *k_verdict_names[VERDICT_COUNT] = {
    "not-run", "incomplete", "pass", "fail", "blocked", "warn", "skip", "unknown"
};

static verdict verdict_of(const case_record *c)
{
    if (!c->executed)
        return VERDICT_NOT_RUN;
    if (!c->have_result)
        return VERDICT_INCOMPLETE;
    switch (c->result)
    {
    case WTT_PASS: return VERDICT_PASS;
    case WTT_FAIL: return VERDICT_FAIL;
    case WTT_BLOCKED: return VERDICT_BLOCKED;
    case WTT_WARN: return VERDICT_WARN;
    case WTT_SKIP: return VERDICT_SKIP;
    default: return VERDICT_UNKNOWN;
    }
}

/* --- running --- */

static void append(char *cmd, size_t cap, const char *fmt, ...)
{
    va_list args;
    size_t len = strlen(cmd);
    va_start(args, fmt);
    vsnprintf(cmd + len, cap - len, fmt, args);
    va_end(args);
}

/*
 * Selectors the framework family has no spelling for.  The report carries them per
 * binary: a run that asked for a sample and did not get one has exercised every case,
 * and reading its counts as a sample would be wrong by orders of magnitude.
 */
#define SELECTOR_SAMPLE 0x1
#define SELECTOR_GROUP  0x2
#define SELECTOR_EXCLUSION 0x4

static void selector_names(int mask, char *out, size_t cap)
{
    out[0] = 0;
    if (mask & SELECTOR_SAMPLE)
        snprintf(out, cap, "sample");
    if (mask & SELECTOR_GROUP)
        snprintf(out + strlen(out), cap - strlen(out), "%sgroup",
                 out[0] ? "," : "");
    if (mask & SELECTOR_EXCLUSION)
        snprintf(out + strlen(out), cap - strlen(out), "%sexclusion",
                 out[0] ? "," : "");
}

/*
 * Whether an entry with a group says anything about a process selecting `sel` (NULL:
 * the whole binary).  A group-level entry covering `sel` itself never reaches here:
 * run_one leaves such a group out instead of spawning it.
 */
static int exclusion_applies(const exclusion *x, const catalog_entry *e, const char *sel)
{
    size_t len;

    if (!x->group[0] || !exclusion_names_binary(x, e))
        return 0;
    if (!sel)
        return 1;
    if (exclusion_covers_group(x, e, sel))
        return x->cases[0] != 0;
    /* an entry for a group under the one selected (a --group naming a parent) */
    len = strlen(sel);
    return !strcmp(e->harness, "wgf") && !_strnicmp(x->group, sel, len)
        && x->group[len] == '\\';
}

/* Each comma-separated item of an entry's cases, trimmed. */
static int next_item(const char **p, char *out, size_t cap)
{
    const char *s = *p, *end;
    size_t n;

    while (*s == ' ' || *s == ',')
        s++;
    if (!*s)
        return 0;
    end = strchr(s, ',');
    if (!end)
        end = s + strlen(s);
    n = (size_t)(end - s);
    while (n && s[n - 1] == ' ')
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    *p = end;
    return 1;
}

/*
 * The exclusions, in the spelling of the harness that has to honour them.  WGF takes
 * "!Group" and "!Group:1-3,7" as positionals and neither runs nor logs what they name;
 * TAEF takes a /select query; gtest a negative --gtest_filter.  Returns the selectors
 * that could not be expressed, which the report names rather than running them quietly.
 */
static int append_exclusions(char *cmd, size_t cap, const catalog_entry *e,
                             const char *sel, char *neg, size_t neg_cap)
{
    int i, ignored = 0;

    neg[0] = 0;
    for (i = 0; i < g_exclusion_count; i++)
    {
        exclusion *x = &g_exclusions[i];
        const char *p = x->cases;
        char item[256];

        if (!exclusion_applies(x, e, sel))
            continue;
        if (!strcmp(e->harness, "wgf"))
        {
            /* The framework has no wildcards; a wildcard entry is matched against
             * the listed groups instead, which only a per-group run has. */
            const char *g = has_wildcard(x->group) ? sel : x->group;

            if (!g || (sel && has_wildcard(x->group) && !exclusion_covers_group(x, e, sel)))
            {
                ignored |= SELECTOR_EXCLUSION;
                continue;
            }
            append(cmd, cap, x->cases[0] ? " \"!%s:%s\"" : " \"!%s\"", g, x->cases);
            x->matched = 1;
        }
        else if (!strcmp(e->harness, "taef"))
        {
            /* a TAEF group is a class; its cases are method names under it */
            if (!x->cases[0])
                snprintf(neg + strlen(neg), neg_cap - strlen(neg),
                         " and not(@Name='%s::*')", x->group);
            else
                while (next_item(&p, item, sizeof(item)))
                    snprintf(neg + strlen(neg), neg_cap - strlen(neg),
                             " and not(@Name='%s::%s')", sel ? sel : x->group, item);
            x->matched = 1;
        }
        else if (!strcmp(e->harness, "gtest"))
        {
            /* a gtest group is "Suite.*"; its cases are test names in the suite */
            if (!x->cases[0])
                snprintf(neg + strlen(neg), neg_cap - strlen(neg), "%s%s",
                         neg[0] ? ":" : "", x->group);
            else
            {
                char suite[256];
                const char *base = sel ? sel : x->group;
                size_t n = strlen(base);

                if (n >= 2 && !strcmp(base + n - 2, ".*"))
                    n -= 2;
                snprintf(suite, sizeof(suite), "%.*s", (int)n, base);
                while (next_item(&p, item, sizeof(item)))
                    snprintf(neg + strlen(neg), neg_cap - strlen(neg), "%s%s.%s",
                             neg[0] ? ":" : "", suite, item);
            }
            x->matched = 1;
        }
        else
            ignored |= SELECTOR_EXCLUSION;      /* wgf9: selects cases, never leaves any out */
    }
    return ignored;
}

/* Case numbers a resumed WGF group skips: everything up to and including the case the
 * previous process died in.  See spawn_group. */
static char g_resume_skip[32];

static int build_command(char *cmd, size_t cap, const catalog_entry *e,
                         const options *o, const char *logdir, const char *tag,
                         const char *group)
{
    int ignored = 0;
    char neg[4096];

    cmd[0] = 0;
    if (strcmp(e->harness, "taef") == 0)
    {
        /* TAEF's out-of-process host is not usable under Wine: TE.exe and
         * te.processhost.exe deadlock on the ncalrpc handshake, both at 0% CPU,
         * and no test ever starts.  In-process execution skips the host entirely.
         * It is Wine-only because it costs coverage: a class whose metadata needs a
         * fresh host is refused under /inproc ("TAEF would need to start a second
         * test host") and every method in it is blocked - 107 of D3DConf_12_Core's
         * on WARP, in ResourceBinding, ResourceCopy and ResourceHeap.
         *
         * No /enableWttLogging: Wex.Logger's WTT device never calls StartTest or
         * EndTest, so the proxy sees no case boundaries and the results come from
         * the console instead - while the .wtl.trace it writes runs to over 100 MB
         * for a single class and filled the test machine's disk mid-sweep. */
        append(cmd, cap, "TE.exe \"%s\"%s", e->binary, dxcts_wine() ? " /inproc" : "");
        ignored |= append_exclusions(cmd, cap, e, group, neg, sizeof(neg));
        if (neg[0])
            /* " and not(...)" each; the leading "and" pairs with the selection */
            append(cmd, cap, " /select:\"(@Name='%s::*')%s\"", group ? group : "*", neg);
        else if (group)
            append(cmd, cap, " /name:%s::*", group);
        /* TAEF selects by name, not by proportion: there is no -Random equivalent. */
        if (o->sample)
            ignored |= SELECTOR_SAMPLE;
    }
    else if (strcmp(e->harness, "gtest") == 0)
    {
        append(cmd, cap, "\"%s\" --gtest_output=json:%s\\%s.json",
               e->binary, logdir, tag);
        ignored |= append_exclusions(cmd, cap, e, group, neg, sizeof(neg));
        if (neg[0])
            append(cmd, cap, " --gtest_filter=%s-%s", group ? group : "*", neg);
        else if (group)
            append(cmd, cap, " --gtest_filter=%s", group);
        if (o->sample)
            ignored |= SELECTOR_SAMPLE;
    }
    else
    {
        /* The group path is positional and must precede the switches. */
        append(cmd, cap, "\"%s\"", e->binary);
        /* The modern generation selects a group by its path; the D3D9-era one by its
         * key, as a switch. */
        if (group)
            append(cmd, cap, strcmp(e->harness, "wgf") ? " -%s" : " \"%s\"", group);
        /* Exclusions are positionals too, after the selection they narrow. */
        ignored |= append_exclusions(cmd, cap, e, group, neg, sizeof(neg));
        if (group && g_resume_skip[0])
            append(cmd, cap, " \"!%s:%s\"", group, g_resume_skip);
        /* The framework's option parser takes the rest of the token as the value and
         * does not strip quotes, so switch values are passed bare.  logdir is kept
         * relative and space-free for that reason. */
        /* The framework logs only failures and skips unless asked for everything,
         * so without -LogAll a passing case produces no log record, no WTTLog call
         * and no event: passes are invisible and the counts are failures only. */
        append(cmd, cap, " -TextLog -LogAll -LogClean -LogPath:%s -LogFileName:%s",
               logdir, tag);
        append(cmd, cap, " -AdapterType:%s",
               o->adapter_type ? o->adapter_type : "all");
        /* The framework's own device selection: REF, HW, SW, NULL or WARP.  WARP is
         * the reference arm on Windows - a complete software rasterizer for D3D11
         * and D3D12 - which is how a failure of the implementation under test is
         * told apart from a failure of the test or of this harness. */
        if (o->src)
            append(cmd, cap, " -Src:%s", o->src);
        /* Every WGF11 binary imports d3d12.dll as well and takes -11on12, which
         * layers its D3D11 device over a D3D12 one; the kit's D3D12 job list is
         * entirely the D3D11 binaries run this way.  Only the modern generation
         * has the switch. */
        if (o->d3d11on12 && !strcmp(e->harness, "wgf") && !strcmp(e->api, "d3d11"))
            append(cmd, cap, " -11on12");
        append(cmd, cap, " -GraphicsFinishTime:%s -QueryTimeout:%s",
               o->graphics_finish_ms ? o->graphics_finish_ms : "1000",
               o->query_timeout_ms ? o->query_timeout_ms : "1000");
        if (o->sample)
            append(cmd, cap, " -Random:%s -Seed:%s", o->sample,
                   o->seed ? o->seed : "1");
    }
    return ignored;
}

#define MAX_GROUPS 512
#define LIST_GROUPS_TIMEOUT_MS 60000

/*
 * Tests spawn helpers, and TerminateProcess only ends the one process it is given.  A
 * job with kill-on-close takes the whole tree down together, so an orphan cannot
 * outlive its run and compete for the GPU with the next one.
 */
/*
 * The token the tests run under.
 *
 * Some tests refuse to run without an elevated token ("This test must be run
 * elevated." - DXGIMultiPresent, and the ETW listeners behind the DXGI present
 * tests), and the HLK always gives them one.  Wine 10 starts every process with a
 * limited administrator token instead, ElevationType 3, so under it the run would be
 * refused wholesale.  Its linked full token is obtainable and CreateProcessAsUser
 * accepts it, with handle inheritance and the job intact; Windows denies that to an
 * unprivileged caller, and there the tests start under whatever token the runner has.
 * The report says which it was.
 */
static HANDLE g_run_token;
static const char *g_elevation = "elevated";

static void acquire_elevation(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    TOKEN_LINKED_TOKEN linked;
    DWORD size;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY,
                          &token))
    {
        g_elevation = "unknown";
        return;
    }
    memset(&elevation, 0, sizeof(elevation));
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                            &size) && elevation.TokenIsElevated)
    {
        CloseHandle(token);
        return;
    }
    g_elevation = "limited";
    memset(&linked, 0, sizeof(linked));
    if (GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &size)
        && linked.LinkedToken)
    {
        memset(&elevation, 0, sizeof(elevation));
        if (GetTokenInformation(linked.LinkedToken, TokenElevation, &elevation,
                                sizeof(elevation), &size)
            && elevation.TokenIsElevated)
        {
            g_run_token = linked.LinkedToken;
            g_elevation = "linked-token";
        }
        else
            CloseHandle(linked.LinkedToken);
    }
    CloseHandle(token);
}

/*
 * Recursive copy for a binary's deployed data.  DirectML.Test.ModelTests reads its
 * models from %TEMP%\directml.test.modeltests.0.0.4, a location the HLK job populates
 * from the kit's MetacommandHlkTestData; without it every method fails with
 * "Invalid model path" before touching the device.
 */
static int copy_tree(const char *from, const char *to)
{
    char pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    int ok = 1;

    CreateDirectoryA(to, NULL);
    snprintf(pattern, sizeof(pattern), "%s\\*", from);
    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(src, sizeof(src), "%s\\%s", from, fd.cFileName);
        snprintf(dst, sizeof(dst), "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= copy_tree(src, dst);
        else
            ok &= CopyFileA(src, dst, FALSE) ? 1 : 0;
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return ok;
}

static void deploy_data(const catalog_entry *e)
{
    char to[MAX_PATH];

    if (!e->deploy_from[0] || !e->deploy_to[0])
        return;
    if (!ExpandEnvironmentStringsA(e->deploy_to, to, sizeof(to)))
        return;
    if (!copy_tree(e->deploy_from, to))
        fprintf(stderr, "dxcts: could not deploy %s to %s for %s\n", e->deploy_from,
                to, e->binary);
}

/*
 * CPU time consumed by everything in the job.  On Windows that is a liveness signal
 * too: WARP is a software rasterizer, and a single mesh-shader case on an ARM64 guest
 * computes for well over ten minutes with nothing to print, which output growth alone
 * reports as wedged.  Not under Wine, where the wedge this watchdog exists for - a host
 * fault re-entering the signal handler forever - burns CPU just as steadily.
 */
static long long job_cpu_time(HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;

    if (!job || dxcts_wine()
        || !QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acct,
                                      sizeof(acct), NULL))
        return 0;
    return acct.TotalUserTime.QuadPart + acct.TotalKernelTime.QuadPart;
}

/*
 * CreateProcess under the run token, suspended so the caller can put it in a job, and
 * at normal priority: a scheduled task - which is how the console session runs this -
 * starts at below-normal, every test inherits that, and Chromium's test suite refuses
 * to run backgrounded ("Check failed: !IsProcessBackgrounded()"), so Dawn's launcher
 * failed all 8192 of its cases on WARP before one had run.
 */
#define START_FLAGS (CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS)

static BOOL start_process(char *cmd, BOOL inherit, const char *cwd, STARTUPINFOA *si,
                          PROCESS_INFORMATION *pi)
{
    if (g_run_token)
    {
        if (CreateProcessAsUserA(g_run_token, NULL, cmd, NULL, NULL, inherit,
                                 START_FLAGS, NULL, cwd, si, pi))
            return TRUE;
        /* Refused (Windows wants SeAssignPrimaryToken for this): run limited, and
         * say so rather than silently. */
        CloseHandle(g_run_token);
        g_run_token = NULL;
        g_elevation = "limited";
    }
    return CreateProcessA(NULL, cmd, NULL, NULL, inherit, START_FLAGS, NULL, cwd,
                          si, pi);
}

static HANDLE make_kill_on_close_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE job = CreateJobObjectA(NULL, NULL);

    if (!job)
        return NULL;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
    return job;
}

/*
 * The per-binary requests, applied before anything is spawned for this binary - the
 * group listing included, since that runs the binary too.  Cleared for a binary that
 * does not ask, or the previous one's requests would still be set when the next one is
 * listed.  Asked for by measurement: a software adapter is what lets DxgTaef's scope
 * setup fixture run, and it hangs the binaries that do not want one.  See the catalog.
 */
static void apply_requests(const catalog_entry *e, const options *o)
{
    int warp = o->src && !_stricmp(o->src, "WARP");

    SetEnvironmentVariableA("DXCTS_SOFTWARE_ADAPTER",
                            e->software_adapter ? "1" : NULL);
    SetEnvironmentVariableA("DXCTS_DWM_OUTPUT", e->dwm_output ? "1" : NULL);
    /*
     * A run asking for WARP hides every other adapter from enumeration - but only
     * for the harnesses that have no way to ask for a device themselves.  TAEF takes
     * the first adapter DXGI reports and the D3D12 suites create on a null adapter,
     * so hiding is the only lever there, and it is set before the listing because
     * that runs the binary too.  The WGF framework has -Src:WARP, and it needs the
     * enumeration intact: its DXGI present tests look up the adapter that owns the
     * monitor's output, and WARP has no outputs, so with everything else hidden
     * every one of them blocks before its first case ("adapter with no outputs").
     */
    SetEnvironmentVariableA("DXCTS_WARP_ONLY",
                            warp && (!strcmp(e->harness, "taef")
                                     || !strcmp(e->harness, "gtest")) ? "1" : NULL);
}

/*
 * Run a listing command with its output captured and bounded.  Some binaries never
 * return from their own -? at all; killing one leaves a listing that stops wherever it
 * got to, which is reported as a failure so the caller runs the whole binary rather
 * than silently running the prefix it managed to print.
 */
static int capture_listing(const char *cmd_in, const char *logdir, const char *cwd,
                           char *path, size_t path_cap)
{
    char cmd[8192];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE handle, job;
    int killed = 0;

    snprintf(path, path_cap, "%s\\groups.tmp", logdir);
    handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;

    snprintf(cmd, sizeof(cmd), "%s", cmd_in);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = handle;
    si.hStdError = handle;
    memset(&pi, 0, sizeof(pi));

    job = make_kill_on_close_job();

    if (!start_process(cmd, TRUE, cwd, &si, &pi))
    {
        if (job)
            CloseHandle(job);
        CloseHandle(handle);
        return 0;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    if (WaitForSingleObject(pi.hProcess, LIST_GROUPS_TIMEOUT_MS) == WAIT_TIMEOUT)
    {
        /* gone before returning, or it still holds the listing file open */
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
        killed = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);
    CloseHandle(handle);
    return !killed;
}

/*
 * The groups from the files -GenerateCSV writes, one per group, named after it with
 * '\\' as '_'.  Group names contain '_' as well, so a name with one in it cannot be
 * turned back into its path - and a group positional the framework does not know
 * runs the whole binary rather than nothing, so a guessed path would rerun all of it
 * once per guess.  Only a listing whose every name is free of '_' is used; each such
 * name is a top-level group, exactly.
 */
static int list_groups_wgf_csv(const catalog_entry *e, const char *logdir,
                               char groups[MAX_GROUPS][256])
{
    char cmd[1024], rel[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    int count = 0, usable = 1;

    snprintf(rel, sizeof(rel), "%s\\groups-csv", logdir);
    if (!GetFullPathNameA(rel, sizeof(dir), dir, NULL))
        return 0;
    CreateDirectoryA(dir, NULL);
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    /* the binary path is relative to the runner's directory, not the listing's */
    if (!GetFullPathNameA(e->binary, sizeof(path), path, NULL))
        return 0;
    snprintf(cmd, sizeof(cmd), "\"%s\" -GenerateCSV", path);
    if (!capture_listing(cmd, logdir, dir, path, sizeof(path)))
        usable = 0;
    DeleteFileA(path);
    find = FindFirstFileA(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            size_t n = strlen(fd.cFileName);
            char file[MAX_PATH];

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            snprintf(file, sizeof(file), "%s\\%s", dir, fd.cFileName);
            DeleteFileA(file);
            if (n <= 4 || _stricmp(fd.cFileName + n - 4, ".csv"))
                continue;
            if (strchr(fd.cFileName, '_') || count >= MAX_GROUPS || n - 4 >= sizeof(groups[0]))
                usable = 0;
            else
                snprintf(groups[count++], sizeof(groups[0]), "%.*s", (int)(n - 4),
                         fd.cFileName);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryA(dir);
    return usable ? count : 0;
}

/*
 * The framework prints its group tree under a "Test groups:" header, one full
 * backslash path per line, indented by depth.  That is the only listing it offers
 * that keeps the paths intact: -GenerateCSV flattens separators into underscores,
 * which group names also contain, so the path cannot be recovered from a file name.
 * A binary with a windowed shell shows that tree in a dialog instead and never
 * returns from -? (DXGISwap); it is listed from its CSV files, where they are exact.
 */
static int list_groups_wgf(const catalog_entry *e, const char *logdir,
                           char groups[MAX_GROUPS][256])
{
    char cmd[1024], path[MAX_PATH], line[512];
    FILE *fh;
    int count = 0, in_tree = 0, capped = 0;

    snprintf(cmd, sizeof(cmd), "\"%s\" -?", e->binary);
    if (!capture_listing(cmd, logdir, NULL, path, sizeof(path)))
        return list_groups_wgf_csv(e, logdir, groups);

    fh = open_text(path);
    if (!fh)
        return 0;
    while (read_text_line(fh, line, sizeof(line)))
    {
        char *p = line;

        if (count >= MAX_GROUPS)
        {
            capped = 1;
            break;
        }
        if (!in_tree)
        {
            if (strstr(line, "Test groups:"))
                in_tree = 1;
            continue;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || !strcmp(p, "Root"))
            continue;
        /* The tree lists parents as well as leaves, and running a parent re-runs
         * everything under it.  A previous entry that is this one's prefix is such a
         * parent, so drop it. */
        if (count > 0)
        {
            size_t prev = strlen(groups[count - 1]);
            if (!strncmp(groups[count - 1], p, prev) && p[prev] == '\\')
                count--;
        }
        /* A truncated path would select a different, broader group, so it is skipped
         * and the group runs as part of the whole-binary fallback instead. */
        if (strlen(p) >= sizeof(groups[0]))
            continue;
        snprintf(groups[count], sizeof(groups[0]), "%s", p);
        count++;
    }
    fclose(fh);
    DeleteFileA(path);
    /* At the cap the tree was cut off mid-way, so the groups found are a prefix rather
     * than the whole binary; running only them would drop the rest with nothing to say
     * so.  Reported as no listing, which runs the binary whole. */
    return capped ? 0 : count;
}

/*
 * TAEF lists one fully-qualified method per line, "<class>::<method>", indented
 * under the binary.  Running a whole TAEF binary as one process is the same bet the
 * WGF binaries lose - and worse here, because TAEF buffers its console output until
 * it exits, so a binary killed at its timeout yields no records at all.  Splitting
 * per class bounds that: the class is everything before the last "::", which holds
 * for a namespaced class name too.
 */
static int list_groups_taef(const catalog_entry *e, const char *logdir,
                            char groups[MAX_GROUPS][256])
{
    char cmd[1024], path[MAX_PATH], line[512], cls[256];
    FILE *fh;
    int count = 0, capped = 0;

    snprintf(cmd, sizeof(cmd), "TE.exe \"%s\"%s /list", e->binary,
             dxcts_wine() ? " /inproc" : "");
    if (!capture_listing(cmd, logdir, NULL, path, sizeof(path)))
        return 0;

    fh = open_text(path);
    if (!fh)
        return 0;
    while (read_text_line(fh, line, sizeof(line)))
    {
        char *p = line, *sep;

        if (count >= MAX_GROUPS)
        {
            capped = 1;
            break;
        }

        while (*p == ' ' || *p == '\t')
            p++;
        sep = strstr(p, "::");
        if (!sep)
            continue;                   /* the binary path, or a class header */
        for (;;)
        {
            char *next = strstr(sep + 2, "::");
            if (!next)
                break;
            sep = next;
        }
        *sep = 0;
        if (strlen(p) >= sizeof(cls))
            continue;
        snprintf(cls, sizeof(cls), "%s", p);
        if (count > 0 && !strcmp(groups[count - 1], cls))
            continue;                   /* methods of one class are listed together */
        snprintf(groups[count], sizeof(groups[0]), "%s", cls);
        count++;
    }
    fclose(fh);
    DeleteFileA(path);
    return capped ? 0 : count;
}

/*
 * gtest lists "Suite." lines followed by indented test names.  A suite is the unit
 * here: Dawn's own launcher runs batches in child processes and gives up on the whole
 * binary when one dies (under D3DMetal the first device test does), so one process per
 * suite bounds a crash to that suite, as the group split does for the other families.
 */
static int list_groups_gtest(const catalog_entry *e, const char *logdir,
                             char groups[MAX_GROUPS][256])
{
    char cmd[1024], path[MAX_PATH], line[512];
    FILE *fh;
    int count = 0, capped = 0;

    snprintf(cmd, sizeof(cmd), "\"%s\" --gtest_list_tests", e->binary);
    if (!capture_listing(cmd, logdir, NULL, path, sizeof(path)))
        return 0;
    fh = open_text(path);
    if (!fh)
        return 0;
    while (read_text_line(fh, line, sizeof(line)))
    {
        size_t len = strlen(line);

        if (count >= MAX_GROUPS)
        {
            capped = 1;
            break;
        }
        /* a suite line: not indented, ends in '.', possibly "Suite.  # TypeParam = ..." */
        if (line[0] == ' ' || line[0] == '\t' || !len)
            continue;
        {
            char *hash = strstr(line, "  #");
            if (hash)
                *hash = 0;
            len = strlen(line);
        }
        if (len < 2 || line[len - 1] != '.' || len + 1 >= sizeof(groups[0]))
            continue;
        line[len - 1] = 0;
        snprintf(groups[count], sizeof(groups[0]), "%s.*", line);
        count++;
    }
    fclose(fh);
    DeleteFileA(path);
    return capped ? 0 : count;
}

/*
 * The D3D9-era framework lists its groups only in its help dialog: -? opens a window
 * whose rich-edit control reads "Tests available:" and then "-Key: description" per
 * group, and it waits there for someone to close it.  The text is read out of the
 * control and the process killed.  A key is what selects the group on the command
 * line ("-Key", or "-Key:1-50,90-" for case numbers within it).
 */
typedef struct
{
    DWORD pid;
    char *text;
    size_t cap;
    int found;
} help_search;

/* Read a line at a time: Wine's rich edit answers WM_GETTEXT with every line break
 * turned into spaces, which leaves no line to find a group key at the start of, while
 * EM_GETLINE gives each line as it is shown.  Both are system messages, which Windows
 * and Wine alike marshal into the other process. */
static BOOL CALLBACK help_control(HWND hwnd, LPARAM param)
{
    help_search *s = (help_search *)param;
    char cls[64], line[1024];
    DWORD_PTR lines = 0, got;
    size_t used = 0;
    WPARAM i;

    if (!GetClassNameA(hwnd, cls, sizeof(cls)) || _strnicmp(cls, "RichEdit", 8))
        return TRUE;
    if (!SendMessageTimeoutA(hwnd, EM_GETLINECOUNT, 0, 0, SMTO_ABORTIFHUNG, 2000, &lines)
        || !lines)
        return TRUE;
    for (i = 0; i < lines; i++)
    {
        *(WORD *)line = sizeof(line) - 1;   /* EM_GETLINE's buffer size, in place */
        got = 0;
        if (!SendMessageTimeoutA(hwnd, EM_GETLINE, i, (LPARAM)line, SMTO_ABORTIFHUNG, 2000,
                                 &got))
            return TRUE;
        if (got > sizeof(line) - 1)
            got = sizeof(line) - 1;
        if (used + got + 2 > s->cap)
        {
            size_t cap = s->cap * 2 > used + got + 2 ? s->cap * 2 : used + got + 4096;
            char *grown = realloc(s->text, cap);

            if (!grown)
                return TRUE;
            s->text = grown;
            s->cap = cap;
        }
        memcpy(s->text + used, line, got);
        used += got;
        s->text[used++] = '\n';
    }
    s->text[used] = 0;
    s->found = strstr(s->text, "Tests available:") != NULL;
    return !s->found;
}

static BOOL CALLBACK help_window(HWND hwnd, LPARAM param)
{
    help_search *s = (help_search *)param;
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == s->pid)
        EnumChildWindows(hwnd, help_control, param);
    return !s->found;
}

static int list_groups_wgf9(const catalog_entry *e, char groups[MAX_GROUPS][256])
{
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    help_search s;
    HANDLE job;
    DWORD waited = 0;
    int count = 0, capped = 0;
    char *line, *end;

    snprintf(cmd, sizeof(cmd), "\"%s\" -?", e->binary);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    memset(&s, 0, sizeof(s));
    job = make_kill_on_close_job();
    if (!start_process(cmd, FALSE, NULL, &si, &pi))
    {
        if (job)
            CloseHandle(job);
        return 0;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    s.pid = pi.dwProcessId;
    while (!s.found && waited < LIST_GROUPS_TIMEOUT_MS
           && WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT)
    {
        waited += 250;
        EnumWindows(help_window, (LPARAM)&s);
    }
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);
    if (!s.found)
    {
        free(s.text);
        return 0;
    }
    line = strstr(s.text, "Tests available:");
    for (line = strchr(line, '\n'); line && *line; line = end)
    {
        char *colon;
        size_t n;

        line++;
        end = strchr(line, '\n');
        if (!end)
            end = line + strlen(line);
        while (*line == '\r')
            line++;
        /* The framework writes its groups at the margin; a test's own options are
         * indented under them, "-Key:value" (MipGen's -PATTERN:pattern) and "-Key: text"
         * alike (scenario's -GradientAlpha: ... (default=1.0)), and as a group key one
         * runs the whole binary. */
        if (*line == ' ' || *line == '\t')
            continue;
        if (!strncmp(line, "Test selection options:", 23))
            break;
        if (*line != '-' || !(colon = memchr(line, ':', (size_t)(end - line))))
            continue;
        n = (size_t)(colon - line - 1);
        if (!n || n >= sizeof(groups[0]) || memchr(line, ' ', (size_t)(colon - line)))
            continue;
        if (count >= MAX_GROUPS)
        {
            capped = 1;
            break;
        }
        memcpy(groups[count], line + 1, n);
        groups[count++][n] = 0;
    }
    free(s.text);
    return capped ? 0 : count;
}

static int list_groups(const catalog_entry *e, const char *logdir,
                       char groups[MAX_GROUPS][256])
{
    if (!strcmp(e->harness, "taef"))
        return list_groups_taef(e, logdir, groups);
    if (!strcmp(e->harness, "gtest"))
        return list_groups_gtest(e, logdir, groups);
    if (!strcmp(e->harness, "wgf9"))
        return list_groups_wgf9(e, groups);
    return list_groups_wgf(e, logdir, groups);
}


/*
 * MTL_ERROR_MODE=nslog stops the host aborting, which is what lets a group finish -
 * but the call it rejected still failed, and the test carries on with whatever it got
 * back.  A case whose expected result is zeroes can then PASS on a resource the host
 * refused to create: a silently wrong result in place of a loud one.  So they are
 * counted, and a pass in a group that has any has not been shown to mean anything.
 *
 * They are counted out of the runner's OWN stderr, not the child's.  Metal and AGX
 * write to the process's Unix fd 2, and the child inherits that from us: redirecting
 * STARTUPINFO.hStdError only moves the Windows handle, so the per-group .err file
 * stays empty while every one of these lands here.  Groups run one at a time, so the
 * bytes written across a spawn belong to that group.
 */
static char g_stderr_path[MAX_PATH];

static void host_errors_init(void)
{
    /* The launcher names this and points the host's fd 2 at it; asking the stderr
     * handle for its path instead does not survive the Win32/Unix split. */
    if (!GetEnvironmentVariableA("DXCTS_HOST_LOG", g_stderr_path,
                                 sizeof(g_stderr_path)))
        g_stderr_path[0] = 0;
}

static long long host_errors_mark(void)
{
    struct _stat64 st;

    if (!g_stderr_path[0])
        return -1;
    if (_stat64(g_stderr_path, &st) != 0)
        return 0;                       /* not written yet: start from the top */
    return st.st_size;
}

/*
 * One line of the host's diagnostics, or something else that happened to land in the
 * same file.  The three shapes are what these writers actually produce: NSLog stamps
 * every line with an ISO date, so a leading "YYYY-" plus "Validation" is a Metal
 * validation report rather than a test's own use of the word; the other two are
 * Metal's assertion text and AGX's own prefix, which carry no timestamp.
 */
static int is_host_validation_error(const char *line)
{
    if (line[0] == '2' && line[4] == '-' && line[7] == '-'
        && strstr(line, "Validation"))
        return 1;
    return strstr(line, "failed assertion") != NULL
        || strstr(line, "AGX:") != NULL;
}

static int host_errors_since(long long mark)
{
    char line[1024];
    FILE *fh;
    int count = 0;

    if (mark < 0 || !g_stderr_path[0])
        return 0;
    fflush(stderr);
    fh = fopen(g_stderr_path, "rb");
    if (!fh)
        return 0;
    if (_fseeki64(fh, mark, SEEK_SET) == 0)
        while (fgets(line, sizeof(line), fh))
            if (is_host_validation_error(line))
                count++;
    fclose(fh);
    return count;
}

/*
 * Where the D3D9-era framework writes its text log: %USERPROFILE%\\dxlogs\\<binary>.log,
 * whatever -LogPath and -LogFileName say, overwritten by every process.  Each process's
 * is moved beside the rest of its output, where the failure reasons are read from.
 */
static int wgf9_log_path(const catalog_entry *e, char *out, size_t cap)
{
    char profile[MAX_PATH], name[MAX_PATH], *dot;
    DWORD n = GetEnvironmentVariableA("USERPROFILE", profile, sizeof(profile));

    if (!n || n >= sizeof(profile))
        return 0;
    snprintf(name, sizeof(name), "%s", base_name(e->binary));
    dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    snprintf(out, cap, "%s\\dxlogs\\%s.log", profile, name);
    return 1;
}

static int spawn_test(const catalog_entry *e, const options *o, const char *logdir,
                      const char *tag, const char *group, run_record *out)
{
    long long mark;
    char cmd[8192], events[MAX_PATH], child_out[MAX_PATH], child_err[MAX_PATH];
    char wgf9_log[MAX_PATH];
    HANDLE out_handle, err_handle;
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    LARGE_INTEGER freq, t0, t1;
    unsigned seconds;
    DWORD wait;
    HANDLE job;

    snprintf(events, sizeof(events), "%s\\%s.events.jsonl", logdir, tag);
    DeleteFileA(events);
    SetEnvironmentVariableA("DXCTS_EVENTS", events);

    out->selectors_ignored |= build_command(cmd, sizeof(cmd), e, o, logdir, tag,
                                            group);
    /* A cut-off command line would run with half its exclusions, or worse, a
     * truncated selector naming a broader group. */
    if (strlen(cmd) >= sizeof(cmd) - 1)
    {
        fprintf(stderr, "dxcts: command line for %s is too long\n", e->binary);
        out->exit_code = (DWORD)-1;
        return 0;
    }

    /*
     * stdout and stderr go to separate files, and it is not only tidiness: TE.exe
     * writes UTF-16 to stdout while the host implementation logs ASCII to stderr, so
     * sharing one handle interleaves single bytes into a two-byte-per-character
     * stream and every character after the first host message is misaligned.  That
     * silently cost whole binaries - D3DConf_12_0_ResourceBarrierDepthStencil ran
     * three cases and was recorded as none, because D3DMetal logged one unsupported
     * feature query into the middle of the results.
     */
    snprintf(child_out, sizeof(child_out), "%s\\%s.out", logdir, tag);
    snprintf(child_err, sizeof(child_err), "%s\\%s.err", logdir, tag);
    out_handle = CreateFileA(child_out, GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    err_handle = CreateFileA(child_err, GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (out_handle != INVALID_HANDLE_VALUE)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_handle;
        si.hStdError = err_handle != INVALID_HANDLE_VALUE ? err_handle : out_handle;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    memset(&pi, 0, sizeof(pi));

    if (!strcmp(e->harness, "wgf9") && wgf9_log_path(e, wgf9_log, sizeof(wgf9_log)))
        DeleteFileA(wgf9_log);
    else
        wgf9_log[0] = 0;

    QueryPerformanceFrequency(&freq);
    mark = host_errors_mark();
    QueryPerformanceCounter(&t0);

    job = make_kill_on_close_job();

    if (!start_process(cmd, out_handle != INVALID_HANDLE_VALUE, NULL, &si, &pi))
    {
        fprintf(stderr, "dxcts: cannot start %s (error %lu)\n", e->binary,
                GetLastError());
        if (job)
            CloseHandle(job);
        if (out_handle != INVALID_HANDLE_VALUE)
            CloseHandle(out_handle);
        if (err_handle != INVALID_HANDLE_VALUE)
            CloseHandle(err_handle);
        out->exit_code = (DWORD)-1;
        return 0;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    seconds = (!o->timeout_given && e->timeout_s > 0)
            ? (unsigned)e->timeout_s : o->timeout_s;
    /*
     * A host fault on a thread Wine cannot turn into an exception - Metal calling
     * through a null function pointer on a libdispatch queue, say - leaves the process
     * spinning in the signal handler forever: full CPU, no further cases.  Waiting out
     * the whole timeout for that costs hours and says only "slow".  So watch the event
     * stream the WTTLog proxy writes: no new case for --stall seconds, with the process
     * still alive, is a wedge, and it is reported as one.
     */
    {
        DWORD slice = 15000, waited = 0, quiet = 0;
        /* Liveness is any growth in what the test writes: the WTTLog event stream
         * for the HLK frameworks, the console for TAEF and gtest - a gtest binary
         * never writes an event, and judged on the stream alone Dawn was reported
         * wedged at 600 s with its output still growing. */
        long long last = event_file_size(events) + event_file_size(child_out)
                       + event_file_size(child_err) + job_cpu_time(job);

        for (;;)
        {
            wait = WaitForSingleObject(pi.hProcess, slice);
            if (wait != WAIT_TIMEOUT)
                break;
            waited += slice;
            if (seconds && waited >= seconds * 1000)
            {
                out->timed_out = 1;
                break;
            }
            if (o->stall_s)
            {
                long long now = event_file_size(events) + event_file_size(child_out)
                              + event_file_size(child_err) + job_cpu_time(job);

                quiet = (now == last) ? quiet + slice : 0;
                last = now;
                if (quiet >= o->stall_s * 1000)
                {
                    out->wedged = 1;
                    break;
                }
            }
        }
    }
    if (out->timed_out || out->wedged)
    {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    GetExitCodeProcess(pi.hProcess, &out->exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);        /* kills anything the test left behind */
    if (out_handle != INVALID_HANDLE_VALUE)
        CloseHandle(out_handle);
    if (err_handle != INVALID_HANDLE_VALUE)
        CloseHandle(err_handle);

    QueryPerformanceCounter(&t1);
    out->seconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;

    if (wgf9_log[0])
    {
        char log[MAX_PATH];

        snprintf(log, sizeof(log), "%s\\%s.log", logdir, tag);
        MoveFileExA(wgf9_log, log, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    out->host_errors = host_errors_since(mark);
    read_events(out, events);
    if (strcmp(e->harness, "taef") == 0)
        read_taef_console(out, child_out);
    else if (strcmp(e->harness, "gtest") == 0)
    {
        char results[MAX_PATH];

        snprintf(results, sizeof(results), "%s\\%s.json", logdir, tag);
        read_gtest_json(out, results);
    }
    return 1;
}

/*
 * Why the failures in a group happened, read back from the group's own log.
 *
 * The event stream carries verdicts but no text, so a report on its own could say that
 * 190508 cases failed and nothing about why - every analysis had to go back to the log
 * tree, and a run whose logs are gone (a -snapshot VM, say) could not be analysed at
 * all.  Tapping WTTLog's varargs Trace to get the text would put the results channel
 * itself at risk, so the reasons are summarised here instead, per group, after it runs.
 *
 * A parameter line ("Format=R32_FLOAT") is not a reason, and neither is a generic
 * wrapper ("SetupTestCase failed.") when something more specific precedes it.
 */
static int reason_is_parameter(const char *l)
{
    const char *p = l;

    if (!isalpha((unsigned char)*p) && *p != '_')
        return 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '[' || *p == ']'
                  || *p == '#'))
        p++;
    while (*p == ' ')
        p++;
    return *p == '=';
}

static int reason_is_noise(const char *l)
{
    static const char *skip[] = { "Summary:", "Start:", "End:", "SKIPPING",
                                  "Failure Breakdown", "Error Breakdown",
                                  "Device string", "WTTLogger", "Restoring ",
                                  "Test group" };
    size_t i;

    if (!*l)
        return 1;
    for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (strncmp(l, skip[i], strlen(skip[i])) == 0)
            return 1;
    for (i = 0; l[i]; i++)
        if (!isdigit((unsigned char)l[i]) && l[i] != ':' && l[i] != '/' && l[i] != ' ')
            return 0;
    return 1;                                   /* a bare number or a timestamp */
}

static int reason_is_wrapper(const char *l)
{
    return strcmp(l, "SetupTestCase failed.") == 0
        || strcmp(l, "Failure Info:") == 0;
}

static void reason_add(failure_reason *out, int *count, const char *text)
{
    int i;

    for (i = 0; i < *count; i++)
        if (strcmp(out[i].text, text) == 0)
        {
            out[i].count++;
            return;
        }
    if (*count < MAX_REASONS)
    {
        snprintf(out[*count].text, sizeof(out[*count].text), "%s", text);
        out[*count].count = 1;
        (*count)++;
    }
}

/* The log is UTF-16; read it as bytes and drop the zeroes rather than pull in a
 * conversion the rest of the runner does not need. */
static int collect_reasons(const char *path, failure_reason *out)
{
    char line[512], best[512];
    int count = 0, have_best = 0, c;
    FILE *fh = fopen(path, "rb");
    size_t n = 0;

    if (!fh)
        return 0;
    line[0] = best[0] = 0;
    while ((c = fgetc(fh)) != EOF)
    {
        if (c == 0 || c == '\r')
            continue;
        if (c != '\n')
        {
            if (n + 1 < sizeof(line))
                line[n++] = (char)c;
            continue;
        }
        line[n] = 0;
        n = 0;
        {
            char *t = line;
            size_t len;

            while (*t == ' ' || *t == '\t')
                t++;
            len = strlen(t);
            while (len && (t[len - 1] == ' ' || t[len - 1] == '\t'))
                t[--len] = 0;
            if (strncmp(t, "Start:", 6) == 0)
                have_best = 0;
            else if (strncmp(t, "End: Fail", 9) == 0)
            {
                reason_add(out, &count, have_best ? best : "(no reason logged)");
                have_best = 0;
            }
            else if (!reason_is_noise(t) && !reason_is_parameter(t))
            {
                if (!have_best || !reason_is_wrapper(t))
                {
                    snprintf(best, sizeof(best), "%s", t);
                    have_best = 1;
                }
            }
        }
    }
    fclose(fh);
    return count;
}

static void tag_of(const catalog_entry *e, const char *group, char *tag, size_t cap)
{
    char *dot;
    size_t i;

    snprintf(tag, cap, "%s", e->binary);
    dot = strrchr(tag, '.');
    if (dot)
        *dot = 0;
    if (!group)
        return;
    strncat(tag, "-", cap - strlen(tag) - 1);
    i = strlen(tag);
    for (; *group && i + 1 < cap; group++)
        tag[i++] = strchr("\\/ :*?\"<>|", *group) ? '_' : *group;
    tag[i] = 0;
}

/*
 * Passes recorded while the host rejected something.  The test carried on with whatever
 * it got back, so a case whose expected result is zeroes can pass on a resource that
 * was never created: these have not been shown to mean anything.
 */
static int tainted_passes(const run_record *r)
{
    int j, count = 0;

    if (!r->host_errors)
        return 0;
    for (j = 0; j < r->case_count; j++)
        if (!r->cases[j].fixture && verdict_of(&r->cases[j]) == VERDICT_PASS)
            count++;
    return count;
}

static int any_failed(const run_record *r)
{
    int j;

    for (j = 0; j < r->case_count; j++)
        if (r->cases[j].have_result && r->cases[j].result == WTT_FAIL)
            return 1;
    return 0;
}

/* A binary run as one process gets the reasons a group would. */
static void collect_binary_reasons(run_record *out, const char *logdir,
                                   const char *tag)
{
    char log[MAX_PATH];

    if (!any_failed(out))
        return;
    snprintf(log, sizeof(log), "%s\\%s.log", logdir, tag);
    out->reason_count = collect_reasons(log, out->reasons);
}

/*
 * Resuming a WGF group past a case that kills its process.
 *
 * A crash inside the implementation under test - WARP's pixel-shader JIT faults on some
 * WGF11Multisample\InputCoverageWithDepth cases, for one - ends the process, and with it
 * every case after the one that crashed.  Those cases are not the implementation's
 * failures and should not cost the run; nor should each crashing case need an entry in
 * an exclusion list.  So the group is started again after it.
 *
 * The crashing case cannot be seen directly: the framework logs a case, start and end
 * together, only when it finishes, so the one that dies leaves no trace in the event
 * stream or the log.  What can be seen is the last case that finished (the log prints
 * its number just before its "End:") and the group's full ordered case list (its own
 * -GenerateCSV).  The case after the last finished one is the one that crashed.  It is
 * recorded as incomplete, named in the report, and the group restarts with every case
 * up to it skipped.
 */
#define MAX_RESUMES 256

static int crashed_exit(const run_record *r)
{
    return !r->timed_out && !r->wedged && r->exit_code >= 0xC0000000u
        && r->exit_code != (DWORD)-1;
}

/* The number of the last case the log recorded as finished, or 0. */
static long last_logged_case(const char *log)
{
    char line[512];
    long prev = -1, last = 0;
    FILE *fh = open_text(log);

    if (!fh)
        return 0;
    while (read_text_line(fh, line, sizeof(line)))
    {
        char *p = line, *end;
        long n;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            continue;
        if (!strncmp(p, "End:", 4))
        {
            if (prev > 0)
                last = prev;
            continue;
        }
        n = strtol(p, &end, 10);
        while (*end == ' ' || *end == '\t')
            end++;
        prev = (end != p && !*end) ? n : -1;
    }
    fclose(fh);
    return last;
}

/* The first case the group enumerates after `after`, from its -GenerateCSV, or 0. */
static long next_case(const catalog_entry *e, const options *o, const char *logdir,
                      const char *tag, const char *group, long after)
{
    char cmd[8192], path[MAX_PATH], csv[MAX_PATH], dir[MAX_PATH], line[512];
    long found = 0;
    size_t i, n;
    FILE *fh;

    build_command(cmd, sizeof(cmd), e, o, logdir, tag, group);
    append(cmd, sizeof(cmd), " -GenerateCSV");
    if (!capture_listing(cmd, logdir, NULL, path, sizeof(path)))
        return 0;
    DeleteFileA(path);
    /* written to the current directory, named after the group with '\' as '_' */
    if (!GetCurrentDirectoryA(sizeof(dir), dir))
        return 0;
    snprintf(csv, sizeof(csv), "%s\\", dir);
    n = strlen(csv);
    for (i = 0; group[i] && n + 5 < sizeof(csv); i++)
        csv[n++] = group[i] == '\\' ? '_' : group[i];
    snprintf(csv + n, sizeof(csv) - n, ".csv");
    fh = fopen(csv, "r");
    if (!fh)
        return 0;
    while (read_text_line(fh, line, sizeof(line)))
    {
        long num = strtol(line, NULL, 10);

        if (num > after)
        {
            found = num;
            break;
        }
    }
    fclose(fh);
    DeleteFileA(csv);
    return found;
}

static int spawn_group(const catalog_entry *e, const options *o, const char *logdir,
                       const char *tag, const char *group, run_record *part)
{
    char attempt_tag[300];
    long skipped_to = 0;
    int ok, resumes = 0;

    g_resume_skip[0] = 0;
    ok = spawn_test(e, o, logdir, tag, group, part);
    snprintf(attempt_tag, sizeof(attempt_tag), "%s", tag);
    /* A sample is drawn afresh by every process, so resuming one would not run the
     * remainder of the same sample. */
    while (ok && crashed_exit(part) && !strcmp(e->harness, "wgf") && !o->sample
           && resumes < MAX_RESUMES)
    {
        char log[MAX_PATH];
        long done, crashed;
        case_record *c;

        snprintf(log, sizeof(log), "%s\\%s.log", logdir, attempt_tag);
        done = last_logged_case(log);
        if (done < skipped_to)
            done = skipped_to;
        crashed = next_case(e, o, logdir, tag, group, done);
        if (!crashed)
            break;              /* died after its last case: nothing left to run */
        c = open_case(part, group, 0);
        if (c)
            c->executed = 1;    /* started, never ended: incomplete */
        snprintf(part->crashed + strlen(part->crashed),
                 sizeof(part->crashed) - strlen(part->crashed), "%s%ld",
                 part->crashed[0] ? "," : "", crashed);
        part->crash_count++;
        if (g_progress)
        {
            fprintf(g_progress, "    crash: %s %s case %ld (exit 0x%08lx); resuming after it\n",
                    e->binary, group, crashed, (unsigned long)part->exit_code);
            fflush(g_progress);
        }
        skipped_to = crashed;
        snprintf(g_resume_skip, sizeof(g_resume_skip), "1-%ld", crashed);
        snprintf(attempt_tag, sizeof(attempt_tag), "%s-r%d", tag, ++resumes);
        part->exit_code = 0;
        ok = spawn_test(e, o, logdir, attempt_tag, group, part);
    }
    g_resume_skip[0] = 0;
    return ok;
}

static void add_applied(run_record *r, const char *group, const exclusion *x)
{
    if (r->applied_count >= r->applied_capacity)
    {
        int want = r->applied_capacity ? r->applied_capacity * 2 : 16;
        applied_exclusion *grown = realloc(r->applied, (size_t)want * sizeof(*grown));

        if (!grown)
            return;
        r->applied = grown;
        r->applied_capacity = want;
    }
    snprintf(r->applied[r->applied_count].group,
             sizeof(r->applied[0].group), "%s", group);
    r->applied[r->applied_count++].entry = x;
}

/* The case-level entries a process selecting `sel` was given, for the report. */
static void record_case_exclusions(run_record *r, const catalog_entry *e, const char *sel)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (exclusion_applies(&g_exclusions[i], e, sel)
            && !(!sel && !strcmp(e->harness, "wgf") && has_wildcard(g_exclusions[i].group)))
            add_applied(r, sel ? sel : g_exclusions[i].group, &g_exclusions[i]);
}

static int run_one(const catalog_entry *e, const options *o, const char *logdir,
                   run_record *out)
{
    static char groups[MAX_GROUPS][256];
    char tag[256];
    int count, i, j, ok = 0;

    memset(out, 0, sizeof(*out));
    out->entry = (catalog_entry *)e;

    apply_requests(e, o);
    deploy_data(e);

    if (!o->per_group || o->group
        || (strcmp(e->harness, "wgf") && strcmp(e->harness, "wgf9")
            && strcmp(e->harness, "taef") && strcmp(e->harness, "gtest")))
    {
        exclusion *x = o->group ? group_exclusion(e, o->group) : NULL;

        if (x)
        {
            x->matched = 1;
            add_applied(out, o->group, x);
            return 1;
        }
        tag_of(e, NULL, tag, sizeof(tag));
        record_case_exclusions(out, e, o->group);
        ok = o->group ? spawn_group(e, o, logdir, tag, o->group, out)
                      : spawn_test(e, o, logdir, tag, NULL, out);
        out->suspect_pass = tainted_passes(out);
        out->slow = o->slow_s && out->seconds > (double)o->slow_s;
        collect_binary_reasons(out, logdir, tag);
        return ok;
    }

    count = list_groups(e, logdir, groups);
    if (count <= 0)
    {
        tag_of(e, NULL, tag, sizeof(tag));
        record_case_exclusions(out, e, NULL);
        ok = spawn_test(e, o, logdir, tag, NULL, out);
        out->suspect_pass = tainted_passes(out);
        out->slow = o->slow_s && out->seconds > (double)o->slow_s;
        collect_binary_reasons(out, logdir, tag);
        return ok;
    }

    /* One process per group: a group that wedges the host costs its own timeout and
     * nothing after it. */
    for (i = 0; i < count; i++)
    {
        run_record part;
        exclusion *x = group_exclusion(e, groups[i]);

        if (x)
        {
            x->matched = 1;
            add_applied(out, groups[i], x);
            continue;
        }
        record_case_exclusions(out, e, groups[i]);
        memset(&part, 0, sizeof(part));
        part.entry = (catalog_entry *)e;
        tag_of(e, groups[i], tag, sizeof(tag));
        if (spawn_group(e, o, logdir, tag, groups[i], &part))
            ok = 1;

        out->seconds += part.seconds;
        out->timed_out |= part.timed_out;
        out->wedged |= part.wedged;
        if (part.exit_code)
            out->exit_code = part.exit_code;
        out->truncated |= part.truncated;
        out->selectors_ignored |= part.selectors_ignored;
        out->crash_count += part.crash_count;
        out->altered += part.altered;
        out->host_errors += part.host_errors;
        out->suspect_pass += tainted_passes(&part);
        if (out->group_count >= out->group_capacity)
        {
            int want = out->group_capacity ? out->group_capacity * 2 : 64;
            group_result *grown = realloc(out->groups,
                                          (size_t)want * sizeof(*grown));
            if (grown)
            {
                out->groups = grown;
                out->group_capacity = want;
            }
        }
        if (out->group_count < out->group_capacity)
        {
            group_result *g = &out->groups[out->group_count++];

            memset(g, 0, sizeof(*g));
            snprintf(g->name, sizeof(g->name), "%s", groups[i]);
            g->seconds = part.seconds;
            g->cases = part.case_count;
            g->timed_out = part.timed_out;
            g->wedged = part.wedged;
            g->exit_code = part.exit_code;
            g->altered = part.altered;
            g->host_errors = part.host_errors;
            snprintf(g->crashed, sizeof(g->crashed), "%s", part.crashed);
            g->crash_count = part.crash_count;
            g->slow = o->slow_s && part.seconds > (double)o->slow_s;
            {
                int j, failed = 0;

                for (j = 0; j < part.case_count; j++)
                    if (part.cases[j].have_result && part.cases[j].result == 2)
                        failed = 1;
                if (!failed)
                    goto no_reasons;
            }
            {
                char log[MAX_PATH];

                snprintf(log, sizeof(log), "%s\\%s.log", logdir, tag);
                g->reason_count = collect_reasons(log, g->reasons);
            }
no_reasons:;
            if (g->slow && !part.timed_out && g_progress)
            {
                fprintf(g_progress, "    slow: %s %s took %.0fs\n", e->binary,
                        groups[i], part.seconds);
                fflush(g_progress);
            }
        }
        for (j = 0; j < part.case_count; j++)
        {
            case_record *dst = open_case(out, part.cases[j].name,
                                         part.cases[j].pid);
            if (!dst)
                break;
            *dst = part.cases[j];
        }
        free(part.cases);
    }
    return ok;
}

/* --- report --- */

static void describe_target(json_writer *w)
{
    const char *mtl_error_mode = getenv("MTL_ERROR_MODE");
    char name[256];
    DWORD size = sizeof(name);
    SYSTEM_INFO info;
    const char *arch = "unknown";

    json_object_begin(w, "target");
    if (GetComputerNameA(name, &size))
        json_str(w, "machine", name);

    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
    default: break;
    }
    json_str(w, "arch", arch);

    json_str(w, "platform", dxcts_wine() ? "wine" : "windows");
    json_str(w, "elevation", g_elevation);
    if (dxcts_wine())
    {
        const char *(CDECL *wine_version)(void) =
            (const char *(CDECL *)(void))(void *)
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version");

        if (wine_version)
            json_str(w, "wine_version", wine_version());
    }
    json_object_end(w);

    /*
     * Everything in the run that changes what a test sees, named so a result is never
     * silently environment-doctored.  The shims that only make a binary load or answer
     * a capability the way D3D11 defines it are not here: these are the ones that
     * alter behaviour, and each says how to tell when it is no longer needed.
     */
    json_array_begin(w, "workarounds");
    json_object_begin(w, NULL);
    json_str(w, "name", "mtl-error-mode");
    /*
     * Metal reads this from the Unix environment, and a PE cannot put anything there:
     * Wine's exec_wineloader execve's the child with its own Unix environ, so the
     * value has to be present when the runner itself is launched, and every test
     * process then inherits it.  Wine imports the Unix environment into the Win32 one
     * at startup, which is why asking for it here works at all.  Reported either way,
     * so a run made without it explains its own hangs.
     */
    json_str(w, "active", mtl_error_mode ? mtl_error_mode : "");
    json_str(w, "what", "MTL_ERROR_MODE=nslog in the test process");
    json_str(w, "why", "Metal calls abort() when a descriptor fails validation, and "
                       "D3DMetal builds them on libdispatch threads Wine cannot turn "
                       "a signal into an exception on, so the process wedges");
    json_bool(w, "alters_results", 0);
    json_object_end(w);
    /*
     * A backend layer's workarounds are the ones that alter what a test sees, so the
     * layer describes them itself and each says whether it is on in this run.  Asked
     * of the DLL the shims will load, from beside the runner.
     */
    {
        typedef int (WINAPI *workaround_fn)(int, const char **, int *, const char **,
                                            const char **, int *);
        workaround_fn fn = (workaround_fn)
            dxcts_backend_proc((const void *)describe_target, "dxcts_backend_workaround");
        const char *name, *what, *why;
        int active, alters, i;

        for (i = 0; fn && fn(i, &name, &active, &what, &why, &alters); i++)
        {
            json_object_begin(w, NULL);
            json_str(w, "name", name);
            json_bool(w, "active", active);
            json_str(w, "what", what);
            json_str(w, "why", why);
            json_bool(w, "alters_results", alters);
            json_object_end(w);
        }
    }
    json_array_end(w);
}


/*
 * Without this, any Metal validation failure aborts the test process on a thread Wine
 * cannot raise an exception on, and the group wedges for its whole timeout.  It is a
 * property of how the run was launched, not something the runner can set for itself.
 */
static void warn_if_metal_will_abort(void)
{
    if (!dxcts_wine())
        return;                         /* Windows: no Metal underneath */
    if (getenv("MTL_ERROR_MODE") || !g_progress)
        return;
    fprintf(g_progress,
            "dxcts: MTL_ERROR_MODE is not set.  On this host a Metal validation\n"
            "       failure calls abort() on a thread Wine cannot turn into an\n"
            "       exception, and the group wedges until its timeout.  Launch with\n"
            "       MTL_ERROR_MODE=nslog in the environment.\n");
}

static void write_report(FILE *fh, run_record *runs, int count, const options *o)
{
    long long totals[VERDICT_COUNT] = { 0 };
    long long total_cases = 0, executed = 0, all_fixtures = 0, all_failed_fixtures = 0;
    long long all_host_errors = 0, suspect_pass = 0, slow_groups = 0;
    long long excluded_binaries = 0, excluded_groups = 0, excluded_case_sets = 0;
    long long crashes = 0;
    json_writer w;
    int i, j;

    json_init(&w, fh);
    json_int(&w, "schema", 1);
    describe_target(&w);

    json_object_begin(&w, "selection");
    json_str(&w, "api", o->api);
    json_str(&w, "harness", o->harness);
    json_str(&w, "binary", o->binary);
    json_str(&w, "group", o->group);
    json_str(&w, "sample", o->sample);
    json_str(&w, "adapter_type", o->adapter_type ? o->adapter_type : "all");
    json_str(&w, "src", o->src);
    json_str(&w, "graphics_finish_ms",
             o->graphics_finish_ms ? o->graphics_finish_ms : "1000");
    json_str(&w, "query_timeout_ms",
             o->query_timeout_ms ? o->query_timeout_ms : "1000");
    json_bool(&w, "per_group", o->per_group);
    json_bool(&w, "no_patches", o->no_patches);
    json_bool(&w, "d3d11on12", o->d3d11on12);
    json_str(&w, "backend", o->backend ? o->backend : "");
    json_str(&w, "exclusion", o->exclusion ? g_exclusion_path : NULL);
    json_int(&w, "exclusion_entries", g_exclusion_count);
    json_object_end(&w);

    json_array_begin(&w, "binaries");
    for (i = 0; i < count; i++)
    {
        run_record *r = &runs[i];
        long long counts[VERDICT_COUNT] = { 0 };
        long long ex = 0, fixtures = 0, failed_fixtures = 0;
        int v;

        json_object_begin(&w, NULL);
        json_str(&w, "binary", r->entry->binary);
        json_str(&w, "harness", r->entry->harness);
        json_str(&w, "api", r->entry->api);
        json_int(&w, "exit_code", (long long)(long)r->exit_code);
        json_bool(&w, "timed_out", r->timed_out);
        json_bool(&w, "wedged", r->wedged);
        json_num(&w, "seconds", r->seconds);
        if (!r->group_count && r->slow)
            json_bool(&w, "slow", 1);
        if (r->truncated)
            json_bool(&w, "cases_truncated", 1);
        if (r->crash_count && !r->group_count)
        {
            json_int(&w, "crashes", r->crash_count);
            json_str(&w, "crashed_cases", r->crashed);
        }
        if (r->excluded)
        {
            json_object_begin(&w, "excluded");
            json_str(&w, "reason", r->excluded->reason);
            json_int(&w, "line", r->excluded->line);
            json_object_end(&w);
            excluded_binaries++;
        }
        if (r->applied_count)
        {
            int k;

            /* What was left out of a binary that did run: whole groups, which were
             * never spawned, and cases the groups that ran were told to skip. */
            json_array_begin(&w, "exclusions");
            for (k = 0; k < r->applied_count; k++)
            {
                const exclusion *x = r->applied[k].entry;

                json_object_begin(&w, NULL);
                json_str(&w, "group", r->applied[k].group);
                if (x->cases[0])
                    json_str(&w, "cases", x->cases);
                json_str(&w, "reason", x->reason);
                json_int(&w, "line", x->line);
                json_object_end(&w);
                if (x->cases[0])
                    excluded_case_sets++;
                else
                    excluded_groups++;
            }
            json_array_end(&w);
        }
        if (r->selectors_ignored)
        {
            char names[64];

            selector_names(r->selectors_ignored, names, sizeof(names));
            json_str(&w, "selectors_ignored", names);
        }
        if (r->have_harness_total)
            json_int(&w, "harness_reported_cases", r->harness_total);
        if (r->reason_count)
        {
            int k;

            json_array_begin(&w, "failure_reasons");
            for (k = 0; k < r->reason_count; k++)
            {
                json_object_begin(&w, NULL);
                json_str(&w, "reason", r->reasons[k].text);
                json_int(&w, "cases", r->reasons[k].count);
                json_object_end(&w);
            }
            json_array_end(&w);
        }

        if (r->group_count)
        {
            int g;

            json_array_begin(&w, "groups");
            for (g = 0; g < r->group_count; g++)
            {
                json_object_begin(&w, NULL);
                json_str(&w, "name", r->groups[g].name);
                json_int(&w, "cases", r->groups[g].cases);
                json_num(&w, "seconds", r->groups[g].seconds);
                json_bool(&w, "timed_out", r->groups[g].timed_out);
                json_bool(&w, "wedged", r->groups[g].wedged);
                /* A group that ends non-zero without timing out crashed, which is a
                 * result about the implementation; a timeout is not. */
                json_int(&w, "exit_code", (int)r->groups[g].exit_code);
                json_int(&w, "altered_draws", r->groups[g].altered);
                json_int(&w, "host_validation_errors", r->groups[g].host_errors);
                json_bool(&w, "slow", r->groups[g].slow);
                if (r->groups[g].crash_count)
                {
                    /* the cases the process died in, each resumed past */
                    json_int(&w, "crashes", r->groups[g].crash_count);
                    json_str(&w, "crashed_cases", r->groups[g].crashed);
                }
                if (r->groups[g].reason_count)
                {
                    int i;

                    json_array_begin(&w, "failure_reasons");
                    for (i = 0; i < r->groups[g].reason_count; i++)
                    {
                        json_object_begin(&w, NULL);
                        json_str(&w, "reason", r->groups[g].reasons[i].text);
                        json_int(&w, "cases", r->groups[g].reasons[i].count);
                        json_object_end(&w);
                    }
                    json_array_end(&w);
                }
                json_object_end(&w);
            }
            json_array_end(&w);
        }

        json_array_begin(&w, "cases");
        for (j = 0; j < r->case_count; j++)
        {
            const case_record *c = &r->cases[j];
            verdict result = verdict_of(c);

            json_object_begin(&w, NULL);
            json_str(&w, "name", c->name);
            json_bool(&w, "executed", c->executed);
            json_str(&w, "result", k_verdict_names[result]);
            if (c->fixture)
                json_bool(&w, "fixture", 1);
            if (c->have_result)
                json_int(&w, "result_code", (long long)c->result);
            json_object_end(&w);

            if (c->fixture)
            {
                fixtures++;
                if (result != VERDICT_PASS)
                    failed_fixtures++;
                continue;
            }
            if (c->executed)
                ex++;
            counts[result]++;
        }
        json_array_end(&w);

        json_object_begin(&w, "counts");
        /* Fixtures are excluded: they are not test methods, and counting a failed
         * one as a failed case is precisely the harness-for-test confusion the
         * report exists to prevent. */
        json_int(&w, "cases", (long long)r->case_count - fixtures);
        json_int(&w, "fixtures", fixtures);
        json_int(&w, "failed_fixtures", failed_fixtures);
        json_int(&w, "executed", ex);
        json_int(&w, "pass", counts[VERDICT_PASS]);
        json_int(&w, "fail", counts[VERDICT_FAIL]);
        json_int(&w, "skip", counts[VERDICT_SKIP]);
        json_int(&w, "blocked", counts[VERDICT_BLOCKED]);
        json_int(&w, "warn", counts[VERDICT_WARN]);
        json_int(&w, "incomplete", counts[VERDICT_INCOMPLETE]);
        json_int(&w, "not_run", counts[VERDICT_NOT_RUN]);
        json_object_end(&w);
        json_object_end(&w);

        total_cases += (long long)r->case_count - fixtures;
        all_fixtures += fixtures;
        all_failed_fixtures += failed_fixtures;
        executed += ex;
        all_host_errors += r->host_errors;
        if (r->group_count)
        {
            for (v = 0; v < r->group_count; v++)
                if (r->groups[v].slow)
                    slow_groups++;
        }
        else if (r->slow)
        {
            slow_groups++;
        }
        suspect_pass += r->suspect_pass;
        crashes += r->crash_count;
        for (v = 0; v < VERDICT_COUNT; v++)
            totals[v] += counts[v];
    }
    json_array_end(&w);

    json_object_begin(&w, "totals");
    json_int(&w, "binaries", count);
    json_int(&w, "cases", total_cases);
    json_int(&w, "fixtures", all_fixtures);
    json_int(&w, "failed_fixtures", all_failed_fixtures);
    json_int(&w, "executed", executed);
    json_int(&w, "pass", totals[VERDICT_PASS]);
    json_int(&w, "fail", totals[VERDICT_FAIL]);
    json_int(&w, "skip", totals[VERDICT_SKIP]);
    json_int(&w, "blocked", totals[VERDICT_BLOCKED]);
    json_int(&w, "warn", totals[VERDICT_WARN]);
    json_int(&w, "incomplete", totals[VERDICT_INCOMPLETE]);
    json_int(&w, "not_run", totals[VERDICT_NOT_RUN]);
    /*
     * Validation failures the run survived because the host was told to report rather
     * than abort, and the passes recorded in those same groups.  A case whose expected
     * result is zeroes can pass on a resource the host refused to create, so a pass
     * counted here has not been shown to mean anything.  Stated rather than hidden:
     * the group finishing still beats a wedge that reports nothing at all.
     */
    json_int(&w, "slow_groups", slow_groups);
    json_int(&w, "host_validation_errors", all_host_errors);
    json_int(&w, "pass_in_groups_with_host_errors", suspect_pass);
    json_int(&w, "crashed_cases_resumed", crashes);
    json_int(&w, "excluded_binaries", excluded_binaries);
    json_int(&w, "excluded_groups", excluded_groups);
    json_int(&w, "excluded_case_sets", excluded_case_sets);
    json_object_end(&w);

    /* An entry whose binary ran without it applying to anything names a group or
     * case that no longer exists, and only lengthens the list. */
    json_array_begin(&w, "stale_exclusions");
    for (i = 0; i < g_exclusion_count; i++)
    {
        const exclusion *x = &g_exclusions[i];

        if (!x->binary_seen || x->matched)
            continue;
        json_object_begin(&w, NULL);
        json_int(&w, "line", x->line);
        json_str(&w, "binary", x->binary);
        json_str(&w, "harness", x->harness[0] ? x->harness : NULL);
        json_str(&w, "group", x->group[0] ? x->group : NULL);
        json_str(&w, "cases", x->cases[0] ? x->cases : NULL);
        json_object_end(&w);
    }
    json_array_end(&w);

    json_object_end(&w);
    fputc('\n', fh);
}

/* --- entry point --- */

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc)
    {
        fprintf(stderr, "dxcts: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++(*i)];
}

static void default_catalog(char *buf, size_t cap)
{
    char exe[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
    {
        snprintf(buf, cap, "suites\\graphics.jsonl");
        return;
    }
    slash = strrchr(exe, '\\');
    if (slash)
        *slash = 0;
    snprintf(buf, cap, "%s\\suites\\graphics.jsonl", exe);
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES)
        snprintf(buf, cap, "%s\\graphics.jsonl", exe);
}

int main(int argc, char **argv)
{
    options o;
    char catalog_path[MAX_PATH];
    char logdir[MAX_PATH];
    const char *mode;
    run_record *runs;
    int i, count = 0;
    FILE *out = stdout;

    if (argc < 2)
    {
        usage();
        return 2;
    }
    mode = argv[1];

    memset(&o, 0, sizeof(o));
    o.timeout_s = DEFAULT_TIMEOUT_S;
    o.slow_s = DEFAULT_SLOW_S;
    o.stall_s = DEFAULT_STALL_S;
    default_catalog(catalog_path, sizeof(catalog_path));
    o.catalog = catalog_path;

    for (i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--catalog")) o.catalog = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--api")) o.api = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--harness")) o.harness = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--binary")) o.binary = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--group")) o.group = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--sample")) o.sample = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--seed")) o.seed = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--adapter-type"))
            o.adapter_type = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--src")) o.src = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--finish-ms"))
            o.graphics_finish_ms = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--query-ms"))
            o.query_timeout_ms = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--out")) o.out = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--stall"))
            o.stall_s = (unsigned)atoi(arg_value(argc, argv, &i));
        else if (!strcmp(argv[i], "--slow"))
            o.slow_s = (unsigned)strtoul(arg_value(argc, argv, &i), NULL, 10);
        else if (!strcmp(argv[i], "--timeout"))
        {
            o.timeout_s = (unsigned)strtoul(arg_value(argc, argv, &i), NULL, 10);
            o.timeout_given = 1;
        }
        else if (!strcmp(argv[i], "--include-skipped")) o.include_skipped = 1;
        else if (!strcmp(argv[i], "--no-patches")) o.no_patches = 1;
        else if (!strcmp(argv[i], "--per-group")) o.per_group = 1;
        else if (!strcmp(argv[i], "--11on12")) o.d3d11on12 = 1;
        else if (!strcmp(argv[i], "--backend")) o.backend = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--exclusion"))
            o.exclusion = arg_value(argc, argv, &i);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage();
            return 0;
        }
        else
        {
            fprintf(stderr, "dxcts: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    /* The shims load dxcts_backend_<name>.dll from beside themselves when this is set;
     * set before the probe too, so its workarounds are listed the same way. */
    SetEnvironmentVariableA("DXCTS_BACKEND", o.backend);

    if (!load_catalog(o.catalog))
        return 1;
    if (o.exclusion && !load_exclusions(o.exclusion))
        return 1;

    if (!strcmp(mode, "run"))
    {
        g_progress = o.out ? stdout : NULL;
        host_errors_init();
        warn_if_metal_will_abort();
    }

    if (!strcmp(mode, "probe"))
    {
        json_writer w;
        json_init(&w, stdout);
        json_int(&w, "schema", 1);
        describe_target(&w);
        json_int(&w, "catalog_binaries", g_catalog_count);
        json_object_end(&w);
        fputc('\n', stdout);
        return 0;
    }

    if (!strcmp(mode, "list"))
    {
        for (i = 0; i < g_catalog_count; i++)
        {
            const catalog_entry *e = &g_catalog[i];
            if (!selected(e, &o))
                continue;
            const exclusion *x = binary_exclusion(e);

            printf("%-46s %-6s %-12s%s%s%s%s\n", e->binary, e->harness, e->api,
                   e->skip[0] ? "  skip: " : "", e->skip,
                   x ? "  excluded: " : "", x ? x->reason : "");
            count++;
        }
        fprintf(stderr, "%d of %d binaries selected\n", count, g_catalog_count);
        return 0;
    }

    if (strcmp(mode, "run"))
    {
        usage();
        return 2;
    }

    /* The shims read this and turn every workaround off; the report records it, so a
     * run made without them is not mistaken for one made with them. */
    SetEnvironmentVariableA("DXCTS_NO_PATCHES", o.no_patches ? "1" : NULL);

    acquire_elevation();

    snprintf(logdir, sizeof(logdir), "dxcts-out");
    CreateDirectoryA(logdir, NULL);

    runs = calloc(MAX_BINARIES, sizeof(*runs));
    if (!runs)
        return 1;

    for (i = 0; i < g_catalog_count; i++)
    {
        catalog_entry *e = &g_catalog[i];
        if (!selected(e, &o))
            continue;
        /* Flushed because a full run takes hours and is watched live. */
        if (g_progress)
        {
            fprintf(g_progress, "[%d] %s ...", count + 1, e->binary);
            fflush(g_progress);
        }
        fflush(stderr);
        mark_binary_seen(e);
        if ((runs[count].excluded = binary_exclusion(e)) != NULL)
        {
            ((exclusion *)runs[count].excluded)->matched = 1;
            runs[count].entry = e;
            if (g_progress)
            {
                fprintf(g_progress, " excluded: %s\n", runs[count].excluded->reason);
                fflush(g_progress);
            }
            count++;
            continue;
        }
        run_one(e, &o, logdir, &runs[count]);
        if (g_progress && runs[count].applied_count)
        {
            fprintf(g_progress, " (%d exclusion%s applied)", runs[count].applied_count,
                    runs[count].applied_count == 1 ? "" : "s");
            fflush(g_progress);
        }
        if (g_progress)
        {
            fprintf(g_progress, " %d cases, %.1fs%s\n", runs[count].case_count,
                    runs[count].seconds,
                    runs[count].wedged
                        ? " (WEDGED - host fault; needs a shim fix, not a bigger budget)"
                        : runs[count].timed_out ? " (TIMED OUT)" : "");
            fflush(g_progress);
        }
        count++;
    }

    if (count == 0)
        fprintf(stderr, "dxcts: no binary in the catalog matched the selection\n");

    if (o.out)
    {
        out = fopen(o.out, "w");
        if (!out)
        {
            fprintf(stderr, "dxcts: cannot write %s\n", o.out);
            free(runs);
            return 1;
        }
    }
    write_report(out, runs, count, &o);
    if (out != stdout)
        fclose(out);
    for (i = 0; i < count; i++)
    {
        free(runs[i].cases);
        free(runs[i].groups);
        free(runs[i].applied);
    }
    free(runs);
    return 0;
}
