/*
 * The catalog of test binaries, and the exclusion lists that keep part of it out of a
 * run.
 */

#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "runner.h"

catalog_entry g_catalog[MAX_BINARIES];
int g_catalog_count;

int load_catalog(const char *path)
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
const char *base_name(const char *path)
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
 * cannot run on the implementation.  group absent excludes the whole binary; cases
 * absent, the whole group.  binary and group take * and ? wildcards, and a WGF group
 * also covers everything under it, as the framework's own "!Group" does.  cases is the
 * WGF case numbers the framework prints, or for TAEF and gtest the method or test names
 * within the group, comma separated.  Every line must say why: a line without a reason
 * is refused, so the list cannot grow entries nobody can justify removing.
 *
 * Fixing the harness is preferred to adding a line here; a line is what is left when
 * the fault is the implementation's and cannot be survived.
 */

exclusion g_exclusions[MAX_EXCLUSIONS];
int g_exclusion_count;
char g_exclusion_path[MAX_PATH];

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

int load_exclusions(const char *arg)
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
exclusion *binary_exclusion(const catalog_entry *e)
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
exclusion *group_exclusion(const catalog_entry *e, const char *group)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (!g_exclusions[i].cases[0] && exclusion_covers_group(&g_exclusions[i], e, group))
            return &g_exclusions[i];
    return NULL;
}

void mark_binary_seen(const catalog_entry *e)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (exclusion_names_binary(&g_exclusions[i], e))
            g_exclusions[i].binary_seen = 1;
}

int selected(const catalog_entry *e, const options *o)
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
int append_exclusions(char *cmd, size_t cap, const catalog_entry *e,
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

void add_applied(run_record *r, const char *group, const exclusion *x)
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
void record_case_exclusions(run_record *r, const catalog_entry *e, const char *sel)
{
    int i;

    for (i = 0; i < g_exclusion_count; i++)
        if (exclusion_applies(&g_exclusions[i], e, sel)
            && !(!sel && !strcmp(e->harness, "wgf") && has_wildcard(g_exclusions[i].group)))
            add_applied(r, sel ? sel : g_exclusions[i].group, &g_exclusions[i]);
}

