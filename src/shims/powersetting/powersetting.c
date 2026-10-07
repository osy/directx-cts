/*
 * The power-setting apiset.
 *
 * DXGIHybridPresent.exe reaches the power accessors through
 * api-ms-win-power-setting-l1-1-0 rather than powrprof directly, and an apiset name
 * is resolved by the schema before any file search, so it cannot be reached by the
 * powrprof shim's own name.  This module exists only to carry that name's length;
 * every export forwards to the powrprof shim, which is where the behaviour lives.
 */

#include <windows.h>

#include "../../common/shim.h"

DXCTS_SHIM_DLLMAIN()
