/*
 * The JSON report.
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/backend.h"
#include "../common/knob.h"
#include "runner.h"

static const char *k_verdict_names[VERDICT_COUNT] = {
    "not-run", "incomplete", "pass", "fail", "blocked", "warn", "skip", "unknown"
};

static void selector_names(int mask, char *out, size_t cap)
{
    out[0] = 0;
    if (mask & SELECTOR_SAMPLE)
        snprintf(out, cap, "sample");
    if (mask & SELECTOR_EXCLUSION)
        snprintf(out + strlen(out), cap - strlen(out), "%sexclusion",
                 out[0] ? "," : "");
}

void describe_target(json_writer *w)
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
void warn_if_metal_will_abort(void)
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

static void write_reasons(json_writer *w, const failure_reason *reasons, int count)
{
    int i;

    if (!count)
        return;
    json_array_begin(w, "failure_reasons");
    for (i = 0; i < count; i++)
    {
        json_object_begin(w, NULL);
        json_str(w, "reason", reasons[i].text);
        json_int(w, "cases", reasons[i].count);
        json_object_end(w);
    }
    json_array_end(w);
}

void write_report(FILE *fh, run_record *runs, int count, const options *o)
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
        if (r->exit_hung)
            json_bool(&w, "exit_hung", 1);
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
        write_reasons(&w, r->reasons, r->reason_count);

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
                if (r->groups[g].exit_hung)
                    json_bool(&w, "exit_hung", 1);
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
                write_reasons(&w, r->groups[g].reasons, r->groups[g].reason_count);
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

