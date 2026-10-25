/*
 * D3DKMT entry points that Wine's gdi32 does not export, plus fallbacks for the two
 * queries the HLK's adapter classification depends on and the adapter open the
 * D3D9-era framework's version check does.
 *
 * Wine's apiset schema maps api-ms-win-dx-d3dkmt-l1-1-* onto gdi32 and only hashes the
 * name up to the last hyphen, so every version of that apiset resolves through the
 * l1-1-0 entry.  Apiset resolution precedes the file search, so a DLL named after the
 * apiset is never consulted; importers are retargeted at this module by a
 * length-preserving import-name rewrite at staging time instead.
 *
 * Everything gdi32 does export is re-exported unchanged so a retargeted binary keeps one
 * import descriptor and nothing is left unbound.  This shim is staged on Windows too,
 * where every forward resolves and the fallbacks are off.
 */

#include <windows.h>

#include "../../common/knob.h"
#include "../../common/shim.h"
#include "../../common/warp.h"

DXCTS_KNOB(patch_adapter_name, "DXCTS_D3DKMT_ADAPTER_NAME_FALLBACK", 1, 0)
DXCTS_KNOB(patch_adapter_info, "DXCTS_D3DKMT_ADAPTER_INFO_FALLBACK", 1, 0)
DXCTS_REQUEST(warp_only, "DXCTS_WARP_ONLY")

typedef LONG (WINAPI *kmt_fn)(void *);

static HMODULE g_gdi32, g_win32u;

static kmt_fn gdi32_proc(const char *name)
{
    return (kmt_fn)dxcts_real_proc(&g_gdi32, "gdi32.dll", name);
}

/* STATUS_PROCEDURE_NOT_FOUND: the caller sees a failed call rather than a crash if a
 * future Wine drops one of these. */
#define KMT_UNAVAILABLE ((LONG)0xC000007AL)

#define FORWARD(name)                                                     \
    LONG WINAPI name(void *arg)                                           \
    {                                                                     \
        static kmt_fn fn;                                                 \
                                                                          \
        if (!fn)                                                          \
            fn = gdi32_proc(#name);                                       \
        return fn ? fn(arg) : KMT_UNAVAILABLE;                            \
    }

/*
 * Wine has these only in win32u, under the kernel-thunk names; Windows has them in
 * gdi32 under their own.  gdi32 is tried first so the platform's own implementation
 * wins wherever it exists.
 */
#define FORWARD_OR_THUNK(name, thunk)                                     \
    LONG WINAPI name(void *arg)                                           \
    {                                                                     \
        static kmt_fn fn;                                                 \
                                                                          \
        if (!fn)                                                          \
            fn = gdi32_proc(#name);                                       \
        if (!fn)                                                          \
            fn = (kmt_fn)dxcts_real_proc(&g_win32u, "win32u.dll", thunk); \
        return fn ? fn(arg) : KMT_UNAVAILABLE;                            \
    }

FORWARD_OR_THUNK(D3DKMTSubmitCommand, "NtGdiDdDDISubmitCommand")
FORWARD_OR_THUNK(D3DKMTSubmitCommandToHwQueue, "NtGdiDdDDISubmitCommandToHwQueue")
FORWARD_OR_THUNK(D3DKMTEnumAdapters3, "NtDxgkEnumAdapters3")

FORWARD(D3DKMTCloseAdapter)
FORWARD(D3DKMTCreateAllocation)
FORWARD(D3DKMTCreateAllocation2)
FORWARD(D3DKMTCreateDevice)
FORWARD(D3DKMTDestroyAllocation)
FORWARD(D3DKMTDestroyDevice)
FORWARD(D3DKMTEnumAdapters)
FORWARD(D3DKMTEnumAdapters2)
FORWARD(D3DKMTEscape)
FORWARD(D3DKMTOpenAdapterFromGdiDisplayName)
FORWARD(D3DKMTOpenAdapterFromLuid)
FORWARD(D3DKMTOpenResource)
FORWARD(D3DKMTOpenResourceFromNtHandle)
FORWARD(D3DKMTQueryResourceInfo)
FORWARD(D3DKMTQueryStatistics)
FORWARD(D3DKMTQueryVideoMemoryInfo)
FORWARD(D3DKMTSetQueuedLimit)
FORWARD(D3DKMTSetVidPnSourceOwner)
FORWARD(D3DKMTWaitForVerticalBlankEvent)

/*
 * The subset of d3dkmthk.h this shim needs.  Declared here rather than included because
 * d3dkmthk.h is a WDK header, not an SDK one, and the cross-compilers do not ship it.
 * The numeric values are from d3dkmthk.h.
 */
enum
{
    KMTQAITYPE_DRIVERVERSION = 13,
    KMTQAITYPE_ADAPTERTYPE   = 15,
};

/* D3DKMT_ADAPTERTYPE's bitfield: RenderSupported | DisplaySupported. */
#define KMT_ADAPTER_RENDER_AND_DISPLAY 0x3

/* D3DKMT_QUERYADAPTERINFO */
typedef struct
{
    UINT hAdapter;
    UINT Type;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
} kmt_query_adapter_info;

/* D3DKMT_OPENADAPTERFROMDEVICENAME */
typedef struct
{
    const WCHAR *pDeviceName;
    UINT hAdapter;
    LUID AdapterLuid;
} kmt_open_from_device_name;

/* D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME.  DeviceName is CCHDEVICENAME wide characters. */
typedef struct
{
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} kmt_open_from_gdi_name;

/* D3DKMT_OPENADAPTERFROMHDC */
typedef struct
{
    HDC hDc;
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} kmt_open_from_hdc;

/* D3DKMT_OPENADAPTERFROMLUID */
typedef struct
{
    LUID AdapterLuid;
    UINT hAdapter;
} kmt_open_from_luid;

/* The primary display's adapter, which is the one a caller of the two opens below
 * means: the framework's device name and its DC both name the display it renders to. */
static LONG open_primary_display(kmt_open_from_gdi_name *gdi)
{
    static kmt_fn from_gdi;

    if (!from_gdi)
        from_gdi = gdi32_proc("D3DKMTOpenAdapterFromGdiDisplayName");
    if (!from_gdi)
        return KMT_UNAVAILABLE;
    ZeroMemory(gdi, sizeof(*gdi));
    lstrcpyW(gdi->DeviceName, L"\\\\.\\DISPLAY1");
    return from_gdi(gdi);
}

/*
 * Wine's implementation only recognises device interface paths it enumerated itself,
 * and the HLK adapter-classification chain feeds it one it did not, leaving the
 * framework with an empty adapter subset ("AdapterNum (0) was not found ... of size
 * (0)").  Falling back to the primary GDI display name yields the same adapter Wine
 * would have returned.
 */
LONG WINAPI D3DKMTOpenAdapterFromDeviceName(void *arg)
{
    static kmt_fn real;
    kmt_open_from_device_name *req = arg;
    kmt_open_from_gdi_name gdi;
    LONG status;

    if (!real)
        real = gdi32_proc("D3DKMTOpenAdapterFromDeviceName");
    status = real ? real(arg) : KMT_UNAVAILABLE;
    if (status >= 0 || !req || !patch_adapter_name() || open_primary_display(&gdi) < 0)
        return status;
    req->hAdapter = gdi.hAdapter;
    req->AdapterLuid = gdi.AdapterLuid;
    return 0;
}

/*
 * Wine's is a stub that always fails.  The D3D9-era framework opens the adapter of a
 * display DC this way to tell a WDDM driver from an XDDM one, and on failure caps every
 * test at Direct3D 9.0 - skipping each group that tests 9.1 (D3D9Ex) and running the
 * rest as 9.0 ("the maximum version for this OS/architecture is 9.0").
 */
LONG WINAPI D3DKMTOpenAdapterFromHdc(void *arg)
{
    static kmt_fn real;
    kmt_open_from_hdc *req = arg;
    kmt_open_from_gdi_name gdi;
    LONG status;

    if (!real)
        real = gdi32_proc("D3DKMTOpenAdapterFromHdc");
    status = real ? real(arg) : KMT_UNAVAILABLE;
    if (status >= 0 || !req || !req->hDc || !patch_adapter_name()
        || open_primary_display(&gdi) < 0)
        return status;
    req->hAdapter = gdi.hAdapter;
    req->AdapterLuid = gdi.AdapterLuid;
    req->VidPnSourceId = gdi.VidPnSourceId;
    return 0;
}

/*
 * The version WARP reports, asked of WARP rather than assumed.
 *
 * When a run is pointed at WARP the tests are not running on the display driver, so
 * answering with the display driver's version describes the wrong device.
 */
static UINT warp_driver_version(void)
{
    static UINT cached;
    static BOOL tried;
    kmt_fn open_luid = gdi32_proc("D3DKMTOpenAdapterFromLuid");
    kmt_fn query = gdi32_proc("D3DKMTQueryAdapterInfo");
    IDXGIAdapter1 *warp;
    DXGI_ADAPTER_DESC1 desc;
    kmt_open_from_luid open;
    kmt_query_adapter_info q;
    UINT version = 0;

    if (tried)
        return cached;
    tried = TRUE;
    if (!open_luid || !query || !(warp = dxcts_warp_adapter()))
        return 0;
    ZeroMemory(&desc, sizeof(desc));
    ZeroMemory(&open, sizeof(open));
    if (SUCCEEDED(warp->lpVtbl->GetDesc1(warp, &desc)))
    {
        open.AdapterLuid = desc.AdapterLuid;
        if (open_luid(&open) == 0)
        {
            ZeroMemory(&q, sizeof(q));
            q.hAdapter = open.hAdapter;
            q.Type = KMTQAITYPE_DRIVERVERSION;
            q.pPrivateDriverData = &version;
            q.PrivateDriverDataSize = sizeof(version);
            if (query(&q) == 0)
                cached = version;
        }
    }
    warp->lpVtbl->Release(warp);
    return cached;
}

/*
 * Wine does not answer KMTQAITYPE_ADAPTERTYPE, and the framework treats the failure as
 * "not a hardware adapter" and drops the adapter from every -AdapterType subset.  It is
 * answered here, but only where the real call declined: on Windows it succeeds and the
 * fallback never runs.
 */
LONG WINAPI D3DKMTQueryAdapterInfo(void *arg)
{
    static kmt_fn real;
    kmt_query_adapter_info *q = arg;
    LONG status;

    if (!real)
        real = gdi32_proc("D3DKMTQueryAdapterInfo");

    status = real ? real(arg) : KMT_UNAVAILABLE;
    if (!q || !q->pPrivateDriverData)
        return status;

    /* A WARP run is not running on the display driver, so the display driver's version
     * would describe the wrong device.  This is a substitution rather than a fallback,
     * so it replaces a successful answer. */
    if (q->Type == KMTQAITYPE_DRIVERVERSION
        && q->PrivateDriverDataSize >= sizeof(UINT)
        && warp_only())
    {
        UINT warp_version = warp_driver_version();

        if (warp_version)
        {
            *(UINT *)q->pPrivateDriverData = warp_version;
            return 0;
        }
    }
    if (status >= 0 || !patch_adapter_info())
        return status;

    if (q->Type == KMTQAITYPE_ADAPTERTYPE && q->PrivateDriverDataSize >= sizeof(UINT))
    {
        *(UINT *)q->pPrivateDriverData = KMT_ADAPTER_RENDER_AND_DISPLAY;
        return 0;
    }
    return status;
}

DXCTS_SHIM_DLLMAIN()
