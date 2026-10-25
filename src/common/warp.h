/*
 * The WARP adapter, for a run pointed at the software rasteriser.  Asked of the
 * platform's own dxgi, which every shim reaches by that name: none of them is staged
 * under it.
 */

#ifndef DXCTS_WARP_H
#define DXCTS_WARP_H

#include <windows.h>
#include <dxgi1_4.h>

#include "shim.h"

/* One reference for the caller, or NULL where the platform has no WARP. */
static inline IDXGIAdapter1 *dxcts_warp_adapter(void)
{
    static HMODULE dxgi;
    HRESULT (WINAPI *create)(UINT, const GUID *, void **) =
        (HRESULT (WINAPI *)(UINT, const GUID *, void **))
        dxcts_real_proc(&dxgi, "dxgi.dll", "CreateDXGIFactory2");
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;

    if (!create || FAILED(create(0, &IID_IDXGIFactory4, (void **)&factory)) || !factory)
        return NULL;
    if (FAILED(factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter1,
                                                (void **)&adapter)))
        adapter = NULL;
    factory->lpVtbl->Release(factory);
    return adapter;
}

#endif
