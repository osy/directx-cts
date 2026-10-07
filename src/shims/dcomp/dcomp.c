/*
 * dcomp proxy for the one export Wine does not have.
 *
 * D3DConf_11_Core.dll statically imports dcomp ordinal 2005, which on Windows is
 * CreatePresentationFactory (read off a 26100 System32 dcomp.dll: Wine's own dcomp
 * stops at ordinal 1042).  An unbound static import stops the whole 39-method D3D11.3
 * conformance binary from loading, so the entry point exists and reports that the
 * presentation manager is unavailable; only the cases that ask for one fail, and the
 * rest of the binary runs.  Everything else forwards to Wine's dcomp.
 */

#include <windows.h>

#include "../../common/shim.h"

HRESULT WINAPI dxcts_create_presentation_factory(
    IUnknown *device, REFIID iid, void **factory)
{
    (void)device; (void)iid;
    if (factory)
        *factory = NULL;
    return E_NOTIMPL;
}

DXCTS_SHIM_DLLMAIN()
