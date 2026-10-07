/*
 * The translation unit for a shim whose .def is its whole implementation: every export
 * is a loader forwarder, so there is nothing to compile but DllMain.
 */

#include <windows.h>

#include "shim.h"

DXCTS_SHIM_DLLMAIN()
