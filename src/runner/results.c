/*
 * Reading back what a test reported: the WTTLog event stream, TAEF's console, gtest's
 * report, the host's own diagnostics and the reasons a group's cases failed.
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "runner.h"

/* Per binary.  WGF11Shader5x alone runs 339,622 cases; at 2^18 it was silently cut to
 * 262,144 and every later group lost its records. */
#define MAX_CASES (1 << 22)

static size_t name_hash(const char *name)
{
    size_t h = 5381;

    while (*name)
        h = h * 33 + (unsigned char)*name++;
    return h;
}

/* Case names repeat (a WGF group logs every case under its own name), so each distinct
 * one is stored once for the life of the run. */
static const char *intern(const char *name)
{
    static char **table;
    static size_t capacity, used;
    size_t h;

    if (used * 2 >= capacity)
    {
        size_t want = capacity ? capacity * 2 : 4096, j, k;
        char **grown = calloc(want, sizeof(*grown));

        if (!grown)
            return NULL;
        for (j = 0; j < capacity; j++)
        {
            if (!table[j])
                continue;
            for (k = name_hash(table[j]) & (want - 1); grown[k]; k = (k + 1) & (want - 1))
                ;
            grown[k] = table[j];
        }
        free(table);
        table = grown;
        capacity = want;
    }
    for (h = name_hash(name) & (capacity - 1); table[h]; h = (h + 1) & (capacity - 1))
        if (!strcmp(table[h], name))
            return table[h];
    table[h] = _strdup(name);
    if (table[h])
        used++;
    return table[h];
}

/*
 * A group name is not unique: the framework runs every parameter combination in a
 * group under that group's name, so WGF11Clipping's six groups produce 155 cases.
 * Each start/end pair is therefore its own record, and an end completes the most
 * recent open record with a matching name from the same process.
 */
case_record *open_case(run_record *r, const char *name, unsigned long pid)
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

void read_events(run_record *r, const char *path)
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
 * One line of a file opened "rb", with NUL bytes and carriage returns dropped.
 *
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
int read_text_line(FILE *fh, char *line, size_t cap)
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

void read_taef_console(run_record *r, const char *path)
{
    FILE *fh = fopen(path, "rb");
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

void read_gtest_json(run_record *r, const char *path)
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

verdict verdict_of(const case_record *c)
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

void host_errors_init(void)
{
    /* The launcher names this and points the host's fd 2 at it; asking the stderr
     * handle for its path instead does not survive the Win32/Unix split. */
    if (!GetEnvironmentVariableA("DXCTS_HOST_LOG", g_stderr_path,
                                 sizeof(g_stderr_path)))
        g_stderr_path[0] = 0;
}

long long host_errors_mark(void)
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

int host_errors_since(long long mark)
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

int collect_reasons(const char *path, failure_reason *out)
{
    char line[512], best[512];
    int count = 0, have_best = 0;
    FILE *fh = fopen(path, "rb");

    if (!fh)
        return 0;
    best[0] = 0;
    while (read_text_line(fh, line, sizeof(line)))
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
        else if (!reason_is_noise(t) && !reason_is_parameter(t)
                 && (!have_best || !reason_is_wrapper(t)))
        {
            snprintf(best, sizeof(best), "%s", t);
            have_best = 1;
        }
    }
    fclose(fh);
    return count;
}

/*
 * Passes recorded while the host rejected something.  The test carried on with whatever
 * it got back, so a case whose expected result is zeroes can pass on a resource that
 * was never created: these have not been shown to mean anything.
 */
int tainted_passes(const run_record *r)
{
    int j, count = 0;

    if (!r->host_errors)
        return 0;
    for (j = 0; j < r->case_count; j++)
        if (!r->cases[j].fixture && verdict_of(&r->cases[j]) == VERDICT_PASS)
            count++;
    return count;
}

int any_failed(const run_record *r)
{
    int j;

    for (j = 0; j < r->case_count; j++)
        if (r->cases[j].have_result && r->cases[j].result == WTT_FAIL)
            return 1;
    return 0;
}

/* A binary run as one process gets the reasons a group would. */
void collect_binary_reasons(run_record *out, const char *logdir,
                            const char *tag)
{
    char log[MAX_PATH];

    if (!any_failed(out))
        return;
    snprintf(log, sizeof(log), "%s\\%s.log", logdir, tag);
    out->reason_count = collect_reasons(log, out->reasons);
}

