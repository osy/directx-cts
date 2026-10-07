/*
 * Knobs and requests: the two things that reach a shim through the environment.
 * Conflating them is how a run ends up with a patch nobody asked for.
 *
 *   requests  what this binary or this run wants, decided by the runner from the
 *             catalog (DXCTS_SOFTWARE_ADAPTER, DXCTS_DWM_OUTPUT, DXCTS_WARP_ONLY).
 *             Declared with DXCTS_REQUEST.  A knob must never veto one: a request
 *             that cannot be honoured has to fail the test rather than quietly leave
 *             it running against a configuration nobody selected.
 *   knobs     whether a workaround for a defect is applied at all.  Declared with
 *             DXCTS_KNOB, named DXCTS_<SHIM>_<WHAT>, overridden per run with
 *             DXCTS_<SHIM>_<WHAT>=0 or =1.
 *
 * A knob's default differs between Wine and Windows because the defects do: almost
 * every workaround here is for something the host D3D implementation gets wrong under
 * Wine, and applying it on Windows would mask a genuine conformance failure instead.
 * DXCTS_NO_PATCHES=1 turns every knob off at once.
 *
 * Workarounds without a knob are the ones the tests cannot launch without on either
 * platform - an unbound static import, or the results channel itself.  Those are
 * marked permanent where they are implemented.
 */

#ifndef DXCTS_KNOB_H
#define DXCTS_KNOB_H

#include <windows.h>

/* Wine exports this from ntdll and Windows does not.  A passive check: nothing is
 * loaded and nothing faults. */
static inline int dxcts_wine(void)
{
    static int cached = -1;

    if (cached < 0)
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");

        cached = ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
    }
    return cached;
}

static inline int dxcts_requested(const WCHAR *name)
{
    return GetEnvironmentVariableW(name, NULL, 0) != 0;
}

static inline int dxcts_knob_lookup(const WCHAR *name, int on_wine, int on_windows)
{
    WCHAR value[16];
    DWORD len;

    if (GetEnvironmentVariableW(L"DXCTS_NO_PATCHES", NULL, 0) != 0)
        return 0;
    len = GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(value[0]));
    if (len && len < sizeof(value) / sizeof(value[0]))
        return value[0] != L'0';
    return dxcts_wine() ? on_wine : on_windows;
}

/*
 * Both are cached predicates.  The value is read once: some of these are consulted per
 * draw call, and the runner sets them before the process starts.
 */
#define DXCTS_KNOB(fn, name, on_wine, on_windows)                            \
    static int fn(void)                                                      \
    {                                                                        \
        static int cached = -1;                                              \
                                                                             \
        if (cached < 0)                                                      \
            cached = dxcts_knob_lookup(L##name, (on_wine), (on_windows));    \
        return cached;                                                       \
    }

#define DXCTS_REQUEST(fn, name)                                              \
    static int fn(void)                                                      \
    {                                                                        \
        static int cached = -1;                                              \
                                                                             \
        if (cached < 0)                                                      \
            cached = dxcts_requested(L##name);                               \
        return cached;                                                       \
    }

#endif
