/*
 * Running a test binary: its command line, its process and job, the watchdog, and the
 * ways a group is split or resumed.
 */

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/backend.h"
#include "../common/knob.h"
#include "runner.h"

/* A test whose log already holds its closing Summary has nothing left to report, so a
 * process that then sits quiet this long is stuck in its own exit, not in a case.
 * CrossOver's DXMT faults in a worker thread's autorelease-pool teardown at exit and
 * never finishes dying; waiting out the full stall for that cost 600 s a group. */
#define EXIT_GRACE_S 30

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

/* Whether a framework log ends in its closing "Summary: Total=" block.  Some
 * frameworks (10L9, the DXGI binaries) write a summary after every sub-test, so a
 * summary only closes the log when no case starts after it.  The log may be UTF-16 or
 * ASCII, so the NUL bytes are dropped before matching. */
static int log_has_summary(const char *path)
{
    static const char want[] = "Summary: Total=";
    char raw[8192], text[sizeof(raw) + 1], *at_summary, *next;
    LARGE_INTEGER size, at;
    HANDLE handle;
    DWORD got = 0, i, n = 0;

    if (!path || !path[0])
        return 0;
    handle = CreateFileA(path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    if (GetFileSizeEx(handle, &size))
    {
        at.QuadPart = size.QuadPart > (LONGLONG)sizeof(raw)
                    ? size.QuadPart - (LONGLONG)sizeof(raw) : 0;
        if (SetFilePointerEx(handle, at, NULL, FILE_BEGIN))
            ReadFile(handle, raw, sizeof(raw), &got, NULL);
    }
    CloseHandle(handle);
    for (i = 0; i < got; i++)
        if (raw[i])
            text[n++] = raw[i];
    text[n] = 0;
    at_summary = strstr(text, want);
    if (!at_summary)
        return 0;
    while ((next = strstr(at_summary + 1, want)) != NULL)
        at_summary = next;
    return strstr(at_summary, "Start: ") == NULL;
}

void append(char *cmd, size_t cap, const char *fmt, ...)
{
    va_list args;
    size_t len = strlen(cmd);
    va_start(args, fmt);
    vsnprintf(cmd + len, cap - len, fmt, args);
    va_end(args);
}

/* Case numbers a resumed WGF group skips: everything up to and including the case the
 * previous process died in.  See spawn_group. */
static char g_resume_skip[32];

/* The case numbers a D3D9-era group process runs, or 0: see spawn_wgf9_group. */
static long g_wgf9_first, g_wgf9_last;

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
        if (group && g_wgf9_first)
            append(cmd, cap, " -%s:%ld-%ld", group, g_wgf9_first, g_wgf9_last);
        else if (group)
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
const char *g_elevation = "elevated";

void acquire_elevation(void)
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

BOOL start_process(char *cmd, BOOL inherit, const char *cwd, STARTUPINFOA *si,
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

/*
 * Tests spawn helpers, and TerminateProcess only ends the one process it is given.  A
 * job with kill-on-close takes the whole tree down together, so an orphan cannot
 * outlive its run and compete for the GPU with the next one.
 */
HANDLE make_kill_on_close_job(void)
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
    char wgf9_log[MAX_PATH], own_log[MAX_PATH];
    HANDLE out_handle, err_handle;
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    LARGE_INTEGER freq, t0, t1;
    unsigned seconds;
    DWORD wait;
    HANDLE job;

    snprintf(events, sizeof(events), "%s\\%s.events.jsonl", logdir, tag);
    snprintf(own_log, sizeof(own_log), "%s\\%s.log", logdir, tag);
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
                if (quiet >= EXIT_GRACE_S * 1000
                    && log_has_summary(wgf9_log[0] ? wgf9_log : own_log))
                {
                    out->exit_hung = 1;
                    break;
                }
                if (quiet >= o->stall_s * 1000)
                {
                    out->wedged = 1;
                    break;
                }
            }
        }
    }
    if (out->timed_out || out->wedged || out->exit_hung)
    {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    GetExitCodeProcess(pi.hProcess, &out->exit_code);
    /* the test completed; the exit code it never got to return is not a crash */
    if (out->exit_hung)
        out->exit_code = 0;
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
    FILE *fh = fopen(log, "rb");

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

static int spawn_wgf9_group(const catalog_entry *e, const options *o, const char *logdir,
                            const char *tag, const char *group, run_record *part);

/* A case the process died in: recorded as started and never ended, and named. */
static void note_crash(run_record *part, const catalog_entry *e, const char *group,
                       long number, const char *next)
{
    case_record *c = open_case(part, group, 0);

    if (c)
        c->executed = 1;
    snprintf(part->crashed + strlen(part->crashed),
             sizeof(part->crashed) - strlen(part->crashed), "%s%ld",
             part->crashed[0] ? "," : "", number);
    part->crash_count++;
    if (g_progress)
    {
        fprintf(g_progress, "    crash: %s %s case %ld (exit 0x%08lx); %s\n", e->binary,
                group, number, (unsigned long)part->exit_code, next);
        fflush(g_progress);
    }
}

static int spawn_group(const catalog_entry *e, const options *o, const char *logdir,
                       const char *tag, const char *group, run_record *part)
{
    char attempt_tag[300];
    long skipped_to = 0;
    int ok, resumes = 0;

    if (!strcmp(e->harness, "wgf9") && !o->sample)
        return spawn_wgf9_group(e, o, logdir, tag, group, part);
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

        snprintf(log, sizeof(log), "%s\\%s.log", logdir, attempt_tag);
        done = last_logged_case(log);
        if (done < skipped_to)
            done = skipped_to;
        crashed = next_case(e, o, logdir, tag, group, done);
        if (!crashed)
            break;              /* died after its last case: nothing left to run */
        note_crash(part, e, group, crashed, "resuming after it");
        skipped_to = crashed;
        snprintf(g_resume_skip, sizeof(g_resume_skip), "1-%ld", crashed);
        snprintf(attempt_tag, sizeof(attempt_tag), "%s-r%d", tag, ++resumes);
        part->exit_code = 0;
        ok = spawn_test(e, o, logdir, attempt_tag, group, part);
    }
    g_resume_skip[0] = 0;
    return ok;
}

/*
 * A D3D9-era group in case-number ranges.
 *
 * StretchRect's first group alone runs past 255,000 cases at 18 a second, more than
 * any timeout that would still catch a hang, so a group goes in ranges of WGF9_CHUNK
 * case numbers, one process each, until a range reaches the group's last case.  The
 * framework repeats a group once per device mode and numbers each repetition from 1,
 * and a range applies to every repetition, so the ranges partition the group: nothing
 * runs twice.  One thing does change at a boundary.  A test that cannot run a block of
 * cases skips the block counted from the case it is at (TexLoad: SkipTestRange(n, n+4)),
 * so a range starting inside such a block makes it skip into cases it would have run:
 * TexLoad in ranges of 7 ran 69 of its 78 cases.  At WGF9_CHUNK apart that is
 * at most a block per boundary, and only in a group larger than one range.
 *
 * The text log says where a range ended: every case is "Start:" then its number, and
 * CleanupTest's number - one past the group's last case, whatever range was asked for -
 * gives the group's size.  A range whose process dies loses the rest of that range only
 * (the case it died in is named, as for WGF); the next range runs.  Resuming within the
 * range is not possible: the restart would have to skip what the dying repetition had
 * done without skipping it in the repetitions after it, and the selection cannot say
 * that.
 */
#define WGF9_CHUNK 25000
#define WGF9_MAX_CHUNKS 400

/* From a D3D9-era text log: the group's last case number (CleanupTest's less one, or
 * -1 if it never ran) and the number of a case that started and never ended, or 0. */
static void scan_wgf9_log(const char *log, long *last_case, long *unfinished)
{
    char line[512];
    long current = 0;
    int numbered = 1, cleanup = 0, open = 0;
    FILE *fh = fopen(log, "rb");

    *last_case = -1;
    *unfinished = 0;
    if (!fh)
        return;
    while (read_text_line(fh, line, sizeof(line)))
    {
        char *p = line, *end;
        long n;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "Start: ", 7))
        {
            numbered = 0;
            open = 1;
            cleanup = !strncmp(p + 7, "CleanupTest,", 12);
            continue;
        }
        if (!strncmp(p, "End:", 4))
        {
            open = 0;
            continue;
        }
        if (numbered || !*p)
            continue;
        n = strtol(p, &end, 10);
        if (end == p || *end)
            continue;
        numbered = 1;
        current = n;
        if (cleanup && n - 1 > *last_case)
            *last_case = n - 1;
    }
    fclose(fh);
    if (open)
        *unfinished = current;
}

/* Every range runs the framework's SetupTest and CleanupTest once per pass, and the
 * first range has recorded them already: a later range's passing pair is not another
 * case (scenario's second range, which holds none of its cases, added six).  One that
 * did not pass is kept, as its range's failure. */
static void drop_repeated_frame_cases(run_record *r, int from)
{
    int i, kept = from;

    for (i = from; i < r->case_count; i++)
    {
        const case_record *c = &r->cases[i];

        if (c->have_result && c->result == WTT_PASS
            && (!strcmp(c->name, "SetupTest") || !strcmp(c->name, "CleanupTest")))
            continue;
        r->cases[kept++] = *c;
    }
    r->case_count = kept;
}

static int spawn_wgf9_group(const catalog_entry *e, const options *o, const char *logdir,
                            const char *tag, const char *group, run_record *part)
{
    long last_case = -1;
    int chunk, ok = 1;

    for (chunk = 0; chunk < WGF9_MAX_CHUNKS; chunk++)
    {
        char attempt_tag[300], log[MAX_PATH];
        long found, unfinished;
        int before = part->case_count, crashed, ran;

        g_wgf9_first = (long)chunk * WGF9_CHUNK + 1;
        g_wgf9_last = g_wgf9_first + WGF9_CHUNK - 1;
        /* the first range keeps the group's own tag, which its reasons are read under */
        if (chunk)
            snprintf(attempt_tag, sizeof(attempt_tag), "%s-c%d", tag, chunk + 1);
        else
            snprintf(attempt_tag, sizeof(attempt_tag), "%s", tag);
        part->exit_code = 0;
        if (!spawn_test(e, o, logdir, attempt_tag, group, part))
            ok = 0;
        ran = part->case_count != before;
        if (chunk)
            drop_repeated_frame_cases(part, before);
        snprintf(log, sizeof(log), "%s\\%s.log", logdir, attempt_tag);
        scan_wgf9_log(log, &found, &unfinished);
        if (found > last_case)
            last_case = found;
        crashed = ok && crashed_exit(part) && unfinished > 0;
        if (crashed)
            note_crash(part, e, group, unfinished, "next range");
        if (!ok || part->timed_out || part->wedged)
            break;
        /* the group's end is known and inside this range, or this range was the last
         * that could tell: a clean exit that never reached CleanupTest, or a death
         * before its first case */
        if ((last_case >= 0 && g_wgf9_last >= last_case) || (!crashed && found < 0) || !ran)
            break;
    }
    g_wgf9_first = g_wgf9_last = 0;
    return ok;
}

/* The binary as one process, or the one group --group named. */
static int run_whole(const catalog_entry *e, const options *o, const char *logdir,
                     const char *group, run_record *out)
{
    char tag[256];
    int ok;

    tag_of(e, NULL, tag, sizeof(tag));
    record_case_exclusions(out, e, group);
    ok = group ? spawn_group(e, o, logdir, tag, group, out)
               : spawn_test(e, o, logdir, tag, NULL, out);
    out->suspect_pass = tainted_passes(out);
    out->slow = o->slow_s && out->seconds > (double)o->slow_s;
    collect_binary_reasons(out, logdir, tag);
    return ok;
}

int run_one(const catalog_entry *e, const options *o, const char *logdir,
            run_record *out)
{
    static char groups[MAX_GROUPS][256];
    char tag[256];
    int count, i, j, ok = 0;

    memset(out, 0, sizeof(*out));
    out->entry = (catalog_entry *)e;

    apply_requests(e, o);
    deploy_data(e);

    if (!o->per_group || o->group)
    {
        exclusion *x = o->group ? group_exclusion(e, o->group) : NULL;

        if (x)
        {
            x->matched = 1;
            add_applied(out, o->group, x);
            return 1;
        }
        return run_whole(e, o, logdir, o->group, out);
    }

    count = list_groups(e, logdir, groups);
    if (count <= 0)
        return run_whole(e, o, logdir, NULL, out);

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
        out->exit_hung |= part.exit_hung;
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
            g->exit_hung = part.exit_hung;
            g->exit_code = part.exit_code;
            g->altered = part.altered;
            g->host_errors = part.host_errors;
            snprintf(g->crashed, sizeof(g->crashed), "%s", part.crashed);
            g->crash_count = part.crash_count;
            g->slow = o->slow_s && part.seconds > (double)o->slow_s;
            if (any_failed(&part))
            {
                char log[MAX_PATH];

                snprintf(log, sizeof(log), "%s\\%s.log", logdir, tag);
                g->reason_count = collect_reasons(log, g->reasons);
            }
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

