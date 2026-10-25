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

#ifndef DXCTS_RUNNER_H
#define DXCTS_RUNNER_H

#include <windows.h>
#include <stdio.h>

#include "../common/json.h"

#define MAX_BINARIES 256

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
    /* finished and wrote its Summary, then never exited; terminated, verdicts kept */
    int exit_hung;
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
    int exit_hung;          /* see group_result */
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

/*
 * Selectors the framework family has no spelling for.  The report carries them per
 * binary: a run that asked for a sample and did not get one has exercised every case,
 * and reading its counts as a sample would be wrong by orders of magnitude.
 */
#define SELECTOR_SAMPLE    0x1
#define SELECTOR_EXCLUSION 0x2

#define MAX_GROUPS 512

/* --- options and progress (main.c) --- */

extern FILE *g_progress;

/* --- the catalog and the exclusion lists (catalog.c) --- */

extern catalog_entry g_catalog[MAX_BINARIES];
extern int g_catalog_count;
extern exclusion g_exclusions[MAX_EXCLUSIONS];
extern int g_exclusion_count;
extern char g_exclusion_path[MAX_PATH];

int load_catalog(const char *path);
const char *base_name(const char *path);
int load_exclusions(const char *arg);
exclusion *binary_exclusion(const catalog_entry *e);
exclusion *group_exclusion(const catalog_entry *e, const char *group);
void mark_binary_seen(const catalog_entry *e);
int selected(const catalog_entry *e, const options *o);
int append_exclusions(char *cmd, size_t cap, const catalog_entry *e,
                      const char *sel, char *neg, size_t neg_cap);
void add_applied(run_record *r, const char *group, const exclusion *x);
void record_case_exclusions(run_record *r, const catalog_entry *e, const char *sel);

/* --- what a test reported: its event stream, console and logs (results.c) --- */

case_record *open_case(run_record *r, const char *name, unsigned long pid);
void read_events(run_record *r, const char *path);
int read_text_line(FILE *fh, char *line, size_t cap);
void read_taef_console(run_record *r, const char *path);
void read_gtest_json(run_record *r, const char *path);
verdict verdict_of(const case_record *c);
void host_errors_init(void);
long long host_errors_mark(void);
int host_errors_since(long long mark);
int collect_reasons(const char *path, failure_reason *out);
int tainted_passes(const run_record *r);
int any_failed(const run_record *r);
void collect_binary_reasons(run_record *out, const char *logdir,
                            const char *tag);

/* --- a binary's groups (listing.c) --- */

int capture_listing(const char *cmd_in, const char *logdir, const char *cwd,
                    char *path, size_t path_cap);
int list_groups(const catalog_entry *e, const char *logdir,
                char groups[MAX_GROUPS][256]);

/* --- running a binary (spawn.c) --- */

extern const char *g_elevation;

void append(char *cmd, size_t cap, const char *fmt, ...);
void acquire_elevation(void);
BOOL start_process(char *cmd, BOOL inherit, const char *cwd, STARTUPINFOA *si,
                   PROCESS_INFORMATION *pi);
HANDLE make_kill_on_close_job(void);
int run_one(const catalog_entry *e, const options *o, const char *logdir,
            run_record *out);

/* --- the JSON report (report.c) --- */

void describe_target(json_writer *w);
void warn_if_metal_will_abort(void);
void write_report(FILE *fh, run_record *runs, int count, const options *o);

#endif
