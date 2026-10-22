/*
 * d3d9 load-fixing proxy (dxc9.dll): everything but the one entry below is a loader
 * forwarder in d3d9.def, which is also the only export list: a dllexport here would
 * add x86's decorated name, numbered by the linker into a free ordinal.
 */
#include <windows.h>

/* Selects how a hybrid (integrated + discrete) system enumerates its adapters; the
 * framework passes its -hybrid mode, 3 by default, and ignores the result.  Wine has
 * one enumeration whatever the mode, so there is nothing to select - leaving it as it
 * is what the call does on a machine that is not hybrid. */
void WINAPI dxcts_d3d9_force_hybrid_enumeration(UINT mode)
{
    (void)mode;
}
