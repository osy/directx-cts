/*
 * The command line, and the loop over the selected binaries.
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/backend.h"
#include "../common/knob.h"
#include "runner.h"

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

/*
 * Where progress goes, or NULL when it must go nowhere.
 *
 * It cannot share a stream with either of the other two writers.  stderr belongs to the
 * host, whose validation failures are counted out of it, and stdout is where the report
 * goes when no --out was given - a progress line there is the first byte of what should
 * be a JSON document.  So with a file to write the report to, progress goes to stdout;
 * without one, there is nowhere left and it is suppressed.
 */
FILE *g_progress;

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
                        : runs[count].timed_out ? " (TIMED OUT)"
                        : runs[count].exit_hung ? " (hung in exit after its summary; terminated)"
                        : "");
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
