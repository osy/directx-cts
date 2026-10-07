/*
 * WTTLog proxy: records the per-case event stream the runner consumes.
 *
 * Both HLK framework generations log through CWTTLogger, so tapping it is the one
 * results channel that covers them.  It answers the question a log file cannot: whether
 * a case executed at all, as opposed to being failed by the framework before its body
 * ran.  (TAEF is not covered: Wex.Logger's WTT device binds only the flat WTTLogTrace*
 * entry points and never calls StartTest or EndTest, so its cases are read back from
 * TE.exe's console output instead.)
 *
 * Permanent, not a knob: it is how results are collected rather than a workaround, which
 * is also why it is staged on Windows.  Without it a run reports no cases at all.
 *
 * Only the CWTTLogger entry points carrying a verdict are tapped; the rest are loader
 * forwarders to the original, so varargs, the vtable and object layout stay exactly as
 * Microsoft built them.  The flat WTTLog* C entry points carry verdicts too and are
 * deliberately left alone - see tools/gen_wttlog_def.py.
 */

#include <windows.h>
#include <stdio.h>

#include "../../common/events.h"
#include "../../common/shim.h"

/*
 * CWTTLogger's methods are C++ instance methods, so both their mangled names and their
 * calling convention are architecture-dependent: __thiscall ("QAE", `this` in ECX) on
 * x86, and the one uniform ABI ("QEAA", `this` as the first argument) everywhere else.
 * The .def maps the real mangled name onto these entry points - tools/gen_wttlog_def.py
 * takes it from the kit - so a tap declared with the wrong convention on x86 would take
 * `this` off the stack and pop the wrong number of bytes.  The names used to look the
 * originals up must match the .def for the same architecture, or the forward silently
 * resolves to nothing and the framework's own log records are lost.
 */
#if defined(__i386__) || defined(_M_IX86)
#define WTT_API __thiscall
#define MANGLED_START_TEST     "?StartTest@CWTTLogger@@QAEJPAGJ@Z"
#define MANGLED_END_TEST       "?EndTest@CWTTLogger@@QAEJPAGK0J@Z"
#define MANGLED_START_TEST_EX  "?StartTestEx@CWTTLogger@@QAEJPAG0K00J@Z"
#define MANGLED_END_TEST_EX    "?EndTestEx@CWTTLogger@@QAEJKPAG000J@Z"
#define MANGLED_START_RESID    "?StartTestWithResID@CWTTLogger@@QAEJIJ@Z"
#define MANGLED_END_RESID      "?EndTestWithResID@CWTTLogger@@QAEJIKPAGJ@Z"
#else
#define WTT_API WINAPI
#define MANGLED_START_TEST     "?StartTest@CWTTLogger@@QEAAJPEAGJ@Z"
#define MANGLED_END_TEST       "?EndTest@CWTTLogger@@QEAAJPEAGK0J@Z"
#define MANGLED_START_TEST_EX  "?StartTestEx@CWTTLogger@@QEAAJPEAG0K00J@Z"
#define MANGLED_END_TEST_EX    "?EndTestEx@CWTTLogger@@QEAAJKPEAG000J@Z"
#define MANGLED_START_RESID    "?StartTestWithResID@CWTTLogger@@QEAAJIJ@Z"
#define MANGLED_END_RESID      "?EndTestWithResID@CWTTLogger@@QEAAJIKPEAGJ@Z"
#endif

typedef LONG (WTT_API *start_ex_fn)(void *, const WCHAR *, const WCHAR *, ULONG,
                                    const WCHAR *, const WCHAR *, LONG);
typedef LONG (WTT_API *end_ex_fn)(void *, ULONG, const WCHAR *, const WCHAR *,
                                  const WCHAR *, const WCHAR *, LONG);
typedef LONG (WTT_API *start_fn)(void *, const WCHAR *, LONG);
typedef LONG (WTT_API *end_fn)(void *, const WCHAR *, ULONG, const WCHAR *, LONG);
typedef LONG (WTT_API *start_resid_fn)(void *, UINT, LONG);
typedef LONG (WTT_API *end_resid_fn)(void *, UINT, ULONG, const WCHAR *, LONG);

static CRITICAL_SECTION g_lock;
static HANDLE g_events = INVALID_HANDLE_VALUE;
static HMODULE g_real;
static LONG g_seq;

/* The kit's own WTTLog, renamed at staging time so this proxy can take its name. */
static void *real_proc(const char *name)
{
    return dxcts_real_proc(&g_real, "dxctswtt.dll", name);
}

/*
 * The case name as a JSON string body.  Truncated rather than dropped if it does not
 * fit: the runner matches a StartTest against an EndTest by name, so an empty name loses
 * the case entirely where a shortened one still pairs up.
 */
static void escape(const WCHAR *in, char *out, size_t cap)
{
    char narrow[1024];
    int wide_len, n = 0;
    size_t i, o = 0;

    out[0] = 0;
    if (!in)
        return;
    /* Halve the input until its UTF-8 form fits.  Both StartTest and EndTest truncate
     * the same name the same way, so a shortened name still pairs up. */
    for (wide_len = (int)lstrlenW(in); wide_len > 0; wide_len /= 2)
    {
        n = WideCharToMultiByte(CP_UTF8, 0, in, wide_len, narrow,
                                (int)sizeof(narrow) - 1, NULL, NULL);
        if (n > 0)
            break;
    }
    narrow[n > 0 ? n : 0] = 0;
    for (i = 0; narrow[i] && o + 7 < cap; i++)
    {
        unsigned char c = (unsigned char)narrow[i];

        if (c == '"' || c == '\\')
        {
            out[o++] = '\\';
            out[o++] = (char)c;
        }
        else if (c < 0x20)
            o += (size_t)sprintf(out + o, "\\u%04x", c);
        else
            out[o++] = (char)c;
    }
    out[o] = 0;
}

static void emit(const char *event, const WCHAR *name, ULONG result, BOOL has_result)
{
    char line[2048], escaped[1536];
    int n;

    if (g_events == INVALID_HANDLE_VALUE)
        return;

    escape(name, escaped, sizeof(escaped));
    if (has_result)
        n = sprintf(line, "{\"event\":\"%s\",\"seq\":%ld,\"pid\":%lu,\"name\":\"%s\","
                          "\"result\":%lu}\n",
                    event, (long)InterlockedIncrement(&g_seq),
                    GetCurrentProcessId(), escaped, result);
    else
        n = sprintf(line, "{\"event\":\"%s\",\"seq\":%ld,\"pid\":%lu,\"name\":\"%s\"}\n",
                    event, (long)InterlockedIncrement(&g_seq),
                    GetCurrentProcessId(), escaped);

    EnterCriticalSection(&g_lock);
    dxcts_events_write(g_events, line, (DWORD)n);
    LeaveCriticalSection(&g_lock);
}

/*
 * The ResID spellings name their case by resource id rather than by string.  Both halves
 * carry the same id in the same argument, so "#<id>" pairs a start with its end exactly
 * as a name does - and left untapped these cases would be reported as never run.
 */
static void emit_resid(const char *event, UINT resid, ULONG result, BOOL has_result)
{
    WCHAR name[24], digits[16];
    int i = 0, o = 1;
    UINT value = resid;

    name[0] = L'#';
    do {
        digits[i++] = (WCHAR)(L'0' + value % 10);
        value /= 10;
    } while (value);
    while (i > 0)
        name[o++] = digits[--i];
    name[o] = 0;
    emit(event, name, result, has_result);
}

LONG WTT_API dxcts_start_test_ex(void *self, const WCHAR *name, const WCHAR *p2,
                                 ULONG p3, const WCHAR *p4, const WCHAR *p5, LONG p6)
{
    static start_ex_fn fn;

    if (!fn)
        fn = (start_ex_fn)real_proc(MANGLED_START_TEST_EX);
    emit("start", name, 0, FALSE);
    return fn ? fn(self, name, p2, p3, p4, p5, p6) : 0;
}

LONG WTT_API dxcts_end_test_ex(void *self, ULONG result, const WCHAR *name,
                               const WCHAR *p3, const WCHAR *p4, const WCHAR *p5,
                               LONG p6)
{
    static end_ex_fn fn;

    if (!fn)
        fn = (end_ex_fn)real_proc(MANGLED_END_TEST_EX);
    emit("end", name, result, TRUE);
    return fn ? fn(self, result, name, p3, p4, p5, p6) : 0;
}

LONG WTT_API dxcts_start_test(void *self, const WCHAR *name, LONG p2)
{
    static start_fn fn;

    if (!fn)
        fn = (start_fn)real_proc(MANGLED_START_TEST);
    emit("start", name, 0, FALSE);
    return fn ? fn(self, name, p2) : 0;
}

LONG WTT_API dxcts_end_test(void *self, const WCHAR *name, ULONG result,
                            const WCHAR *p3, LONG p4)
{
    static end_fn fn;

    if (!fn)
        fn = (end_fn)real_proc(MANGLED_END_TEST);
    emit("end", name, result, TRUE);
    return fn ? fn(self, name, result, p3, p4) : 0;
}

LONG WTT_API dxcts_start_test_resid(void *self, UINT resid, LONG p2)
{
    static start_resid_fn fn;

    if (!fn)
        fn = (start_resid_fn)real_proc(MANGLED_START_RESID);
    emit_resid("start", resid, 0, FALSE);
    return fn ? fn(self, resid, p2) : 0;
}

LONG WTT_API dxcts_end_test_resid(void *self, UINT resid, ULONG result,
                                  const WCHAR *p3, LONG p4)
{
    static end_resid_fn fn;

    if (!fn)
        fn = (end_resid_fn)real_proc(MANGLED_END_RESID);
    emit_resid("end", resid, result, TRUE);
    return fn ? fn(self, resid, result, p3, p4) : 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        dxcts_shim_attach(inst);
        InitializeCriticalSection(&g_lock);
        g_events = dxcts_events_open();
    }
    return TRUE;
}
