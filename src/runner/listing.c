/*
 * Asking a test binary for its groups, so each can run as its own process.
 */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/backend.h"
#include "../common/knob.h"
#include "runner.h"

#define LIST_GROUPS_TIMEOUT_MS 60000

/*
 * Run a listing command with its output captured and bounded.  Some binaries never
 * return from their own -? at all; killing one leaves a listing that stops wherever it
 * got to, which is reported as a failure so the caller runs the whole binary rather
 * than silently running the prefix it managed to print.
 */
int capture_listing(const char *cmd_in, const char *logdir, const char *cwd,
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

    fh = fopen(path, "rb");
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

    fh = fopen(path, "rb");
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
    fh = fopen(path, "rb");
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

int list_groups(const catalog_entry *e, const char *logdir,
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


