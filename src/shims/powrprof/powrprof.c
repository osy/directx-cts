/*
 * The power-scheme value accessors DXGIHybridPresent.exe reads the hybrid-GPU
 * preference through.  Failing them makes it abandon setup before any test case runs.
 *
 * Wine has no implementation of most of these, and the two it does export
 * (PowerReadACValueIndex, PowerWriteACValueIndex) are stubs that return ERROR_SUCCESS
 * without writing the caller's out-parameter, which leaves the index uninitialised.
 * So the readers here report a zero index - the "no preference" default - and the
 * writers accept and discard.  The AC and DC halves of each pair are answered the same
 * way, since a caller that wrote one and read back the other would otherwise see them
 * disagree for no reason it could act on.
 *
 * Importers reach this module through a length-preserving import-name rewrite.
 */

#include <windows.h>

#include "../../common/shim.h"

#define PWR_API __declspec(dllexport) WINAPI

#define READ_INDEX(name)                                                   \
    DWORD PWR_API name(HKEY root, const GUID *scheme, const GUID *subgroup, \
                       const GUID *setting, LPDWORD value)                 \
    {                                                                      \
        (void)root; (void)scheme; (void)subgroup; (void)setting;           \
        if (value)                                                         \
            *value = 0;                                                    \
        return ERROR_SUCCESS;                                              \
    }

READ_INDEX(PowerReadACValueIndex)
READ_INDEX(PowerReadDCValueIndex)
READ_INDEX(PowerReadACDefaultIndex)
READ_INDEX(PowerReadDCDefaultIndex)

#define READ_BOUND(name)                                                   \
    DWORD PWR_API name(HKEY root, const GUID *subgroup,                    \
                       const GUID *setting, LPDWORD value)                 \
    {                                                                      \
        (void)root; (void)subgroup; (void)setting;                         \
        if (value)                                                         \
            *value = 0;                                                    \
        return ERROR_SUCCESS;                                              \
    }

READ_BOUND(PowerReadValueMax)
READ_BOUND(PowerReadValueMin)

#define WRITE_INDEX(name)                                                  \
    DWORD PWR_API name(HKEY root, const GUID *scheme, const GUID *subgroup, \
                       const GUID *setting, DWORD value)                   \
    {                                                                      \
        (void)root; (void)scheme; (void)subgroup; (void)setting;           \
        (void)value;                                                       \
        return ERROR_SUCCESS;                                              \
    }

WRITE_INDEX(PowerWriteACValueIndex)
WRITE_INDEX(PowerWriteDCValueIndex)

DXCTS_SHIM_DLLMAIN()
