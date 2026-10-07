/*
 * Boilerplate every shim DLL shares.
 *
 * Permanent, not a knob: without it a crash becomes something the runner cannot tell
 * from a hang.  Under Wine an unhandled exception is handed to winedbg, which attaches
 * without ever signalling, so the faulting thread never returns and the process wedges
 * until its timeout instead of dying.  SEM_NOGPFAULTERRORBOX short-circuits that filter
 * before the debugger is started, and ntdll terminates the process instead.  On Windows
 * the same flag suppresses the WER dialog, which would stall an unattended run
 * identically.
 */

#ifndef DXCTS_SHIM_H
#define DXCTS_SHIM_H

#include <windows.h>

static inline void dxcts_shim_attach(HINSTANCE inst)
{
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    DisableThreadLibraryCalls(inst);
}

/*
 * An entry point of the module this shim stands in for, resolved once.  Each shim is
 * staged under a name of its own and importers are retargeted at it, so loading the
 * name it replaces reaches the platform's own implementation rather than recursing
 * into this one.
 */
static inline void *dxcts_real_proc(HMODULE *cache, const char *module,
                                    const char *name)
{
    if (!*cache)
        *cache = LoadLibraryA(module);
    return *cache ? (void *)GetProcAddress(*cache, name) : NULL;
}

/*
 * Every shim's DllMain is this.  A shim that needs more work at load time defines its
 * own and calls dxcts_shim_attach from it.
 */
#define DXCTS_SHIM_DLLMAIN()                                        \
    BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) \
    {                                                               \
        (void)reserved;                                             \
        if (reason == DLL_PROCESS_ATTACH)                           \
            dxcts_shim_attach(inst);                                \
        return TRUE;                                                \
    }

#endif
