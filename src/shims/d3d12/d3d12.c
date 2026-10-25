/*
 * d3d12 shim: the two exports Wine's d3d12 does not resolve usefully, the WARP adapter
 * substitution a WARP run needs, and the hand-off of device creation to a backend
 * compatibility layer (common/backend.h), which is where everything the host D3D12
 * implementation gets wrong is handled.
 *
 * This shim is staged on Windows as well as under Wine, so nothing here changes
 * behaviour on a conformant runtime: every export is forwarded to the real
 * implementation unless a request or a backend layer says otherwise.
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../../common/backend.h"
#include "../../common/knob.h"
#include "../../common/shim.h"
#include "../../common/warp.h"

typedef HRESULT (WINAPI *get_interface_fn)(const GUID *clsid, const GUID *iid,
                                           void **out);

/*
 * Set by the runner for a WARP run.  Hiding every other adapter from DXGI enumeration
 * is not enough on its own: D3D12CreateDevice(NULL, ...) takes the default adapter
 * internally without enumerating anything, so the D3D12 suites would still land on the
 * machine's GPU driver.
 */
DXCTS_REQUEST(warp_only, "DXCTS_WARP_ONLY")

static HMODULE g_real_d3d12;

HRESULT WINAPI dxcts_d3d12_create_device(void *adapter, UINT min_feature_level,
                                         const GUID *iid, void **device)
{
    static dxcts_d3d12_create_fn fn;
    static dxcts_backend_d3d12_create_fn backend;
    static int looked_up;
    IDXGIAdapter1 *warp = NULL;
    HRESULT hr;

    if (!fn)
        fn = (dxcts_d3d12_create_fn)dxcts_real_proc(&g_real_d3d12, "d3d12.dll",
                                                    "D3D12CreateDevice");
    if (!fn)
        return E_FAIL;
    if (!looked_up)
    {
        backend = (dxcts_backend_d3d12_create_fn)
            dxcts_backend_proc((const void *)dxcts_d3d12_create_device,
                               "dxcts_backend_d3d12_create_device");
        looked_up = 1;
    }
    if (!adapter && warp_only())
        adapter = warp = dxcts_warp_adapter();
    hr = backend ? backend(fn, adapter, min_feature_level, iid, device)
                 : fn(adapter, min_feature_level, iid, device);
    if (warp)
        warp->lpVtbl->Release(warp);
    return hr;
}

/* PIX capture plumbing.  Permanent, not a knob: an unbound static import stops the
 * image loading, and these are only reached with a PIX capture layer attached, which an
 * unattended conformance run never has. */

void * WINAPI D3D12PIXGetThreadInfo(void)
{
    return NULL;
}

UINT64 WINAPI D3D12PIXEventsReplaceBlock(void *ctx, BOOL get_earliest)
{
    (void)ctx; (void)get_earliest;
    return 0;
}

/*
 * D3D12GetInterface lives in d3d12core, not d3d12, so the import needs retargeting
 * here.  The real one is used whenever the process has a d3d12core that provides it -
 * which is the case on Windows, where this shim is staged too.
 *
 * Under Wine there is only a stub that raises "unimplemented function", an unhandled
 * exception that kills the process, and D3DConf_12_IndependentDevices died with no case
 * run at all.  Reporting that the interface is unavailable is what a runtime without it
 * does, and the caller then handles the absence.  d3d12core is deliberately not loaded
 * to find out: loading Wine's would find the stub, and calling it is what raises.
 */
HRESULT WINAPI dxcts_d3d12_get_interface(const GUID *clsid, const GUID *iid,
                                         void **out)
{
    static get_interface_fn fn;
    static int looked_up;

    if (!looked_up)
    {
        HMODULE core = GetModuleHandleA("d3d12core.dll");

        if (core && !dxcts_wine())
            fn = (get_interface_fn)(void *)
                 GetProcAddress(core, "D3D12GetInterface");
        looked_up = 1;
    }
    if (fn)
        return fn(clsid, iid, out);
    if (out)
        *out = NULL;
    return E_NOINTERFACE;
}

DXCTS_SHIM_DLLMAIN()
