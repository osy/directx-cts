/*
 * D3DKMT entry points that Wine's gdi32 does not export, plus fallbacks for the two
 * queries the HLK's adapter classification depends on.
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
#include <stddef.h>
#include <dxgi1_4.h>

#include "../../common/knob.h"
#include "../../common/shim.h"

#define KMT_API __declspec(dllexport) WINAPI

DXCTS_KNOB(patch_adapter_name, "DXCTS_D3DKMT_ADAPTER_NAME_FALLBACK", 1, 0)
DXCTS_KNOB(patch_adapter_info, "DXCTS_D3DKMT_ADAPTER_INFO_FALLBACK", 1, 0)
DXCTS_REQUEST(warp_only, "DXCTS_WARP_ONLY")

typedef LONG (WINAPI *kmt_fn)(void *);

static kmt_fn resolve(const char *module, const char *name)
{
    HMODULE lib = GetModuleHandleA(module);

    if (!lib)
        lib = LoadLibraryA(module);
    return lib ? (kmt_fn)(void *)GetProcAddress(lib, name) : NULL;
}

/* STATUS_PROCEDURE_NOT_FOUND: the caller sees a failed call rather than a crash if a
 * future Wine drops one of these. */
#define KMT_UNAVAILABLE ((LONG)0xC000007AL)

#define FORWARD(name)                                                  \
    LONG KMT_API name(void *arg)                                       \
    {                                                                  \
        static kmt_fn fn;                                              \
        static BOOL tried;                                             \
                                                                       \
        if (!tried) { fn = resolve("gdi32.dll", #name); tried = TRUE; } \
        return fn ? fn(arg) : KMT_UNAVAILABLE;                         \
    }

/*
 * Wine has these only in win32u, under the kernel-thunk names; Windows has them in
 * gdi32 under their own.  gdi32 is tried first so the platform's own implementation
 * wins wherever it exists.
 */
#define FORWARD_OR_THUNK(name, thunk)                                  \
    LONG KMT_API name(void *arg)                                       \
    {                                                                  \
        static kmt_fn fn;                                              \
        static BOOL tried;                                             \
                                                                       \
        if (!tried)                                                    \
        {                                                              \
            fn = resolve("gdi32.dll", #name);                          \
            if (!fn)                                                   \
                fn = resolve("win32u.dll", thunk);                     \
            tried = TRUE;                                              \
        }                                                              \
        return fn ? fn(arg) : KMT_UNAVAILABLE;                         \
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
FORWARD(D3DKMTOpenAdapterFromHdc)
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

/* D3DKMT_OPENADAPTERFROMLUID */
typedef struct
{
    LUID AdapterLuid;
    UINT hAdapter;
} kmt_open_from_luid;

/*
 * Wine's implementation only recognises device interface paths it enumerated itself,
 * and the HLK adapter-classification chain feeds it one it did not, leaving the
 * framework with an empty adapter subset ("AdapterNum (0) was not found ... of size
 * (0)").  Falling back to the primary GDI display name yields the same adapter Wine
 * would have returned.
 */
LONG KMT_API D3DKMTOpenAdapterFromDeviceName(void *arg)
{
    static kmt_fn real, from_gdi;
    static BOOL tried;
    kmt_open_from_device_name *req = arg;
    kmt_open_from_gdi_name gdi;
    LONG status;

    if (!tried)
    {
        real = resolve("gdi32.dll", "D3DKMTOpenAdapterFromDeviceName");
        from_gdi = resolve("gdi32.dll", "D3DKMTOpenAdapterFromGdiDisplayName");
        tried = TRUE;
    }

    status = real ? real(arg) : KMT_UNAVAILABLE;
    if (status >= 0 || !from_gdi || !req || !patch_adapter_name())
        return status;

    ZeroMemory(&gdi, sizeof(gdi));
    lstrcpyW(gdi.DeviceName, L"\\\\.\\DISPLAY1");
    status = from_gdi(&gdi);
    if (status < 0)
        return status;

    req->hAdapter = gdi.hAdapter;
    req->AdapterLuid = gdi.AdapterLuid;
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
    static const GUID iid_factory4 =
        { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf,0x0c,0x21,0xca,0x39,0xe5,0x16,0x8a } };
    static const GUID iid_adapter1 =
        { 0x29038f61, 0x3839, 0x4626, { 0x91,0xfd,0x08,0x68,0x79,0x01,0x1a,0x05 } };
    HRESULT (WINAPI *create)(UINT, const GUID *, void **);
    HRESULT (WINAPI *enum_warp)(void *, const GUID *, void **);
    HRESULT (WINAPI *get_desc1)(void *, void *);
    void *factory = NULL, *warp = NULL, **vtable;
    kmt_fn open_luid, query;
    DXGI_ADAPTER_DESC1 desc;
    kmt_query_adapter_info q;
    kmt_open_from_luid open;
    HMODULE dxgi;
    UINT version = 0;

    if (tried)
        return cached;
    tried = TRUE;
    dxgi = LoadLibraryA("dxgi.dll");
    open_luid = resolve("gdi32.dll", "D3DKMTOpenAdapterFromLuid");
    query = resolve("gdi32.dll", "D3DKMTQueryAdapterInfo");
    if (!dxgi || !open_luid || !query)
        return 0;
    create = (HRESULT (WINAPI *)(UINT, const GUID *, void **))(void *)
             GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (!create || FAILED(create(0, &iid_factory4, &factory)) || !factory)
        return 0;

    vtable = *(void ***)factory;
    enum_warp = (HRESULT (WINAPI *)(void *, const GUID *, void **))
        vtable[offsetof(IDXGIFactory4Vtbl, EnumWarpAdapter) / sizeof(void *)];
    if (SUCCEEDED(enum_warp(factory, &iid_adapter1, &warp)) && warp)
    {
        void **adapter_vtable = *(void ***)warp;

        get_desc1 = (HRESULT (WINAPI *)(void *, void *))
            adapter_vtable[offsetof(IDXGIAdapter1Vtbl, GetDesc1) / sizeof(void *)];
        ZeroMemory(&desc, sizeof(desc));
        if (SUCCEEDED(get_desc1(warp, &desc)))
        {
            ZeroMemory(&open, sizeof(open));
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
        ((ULONG (WINAPI *)(void *))adapter_vtable[2])(warp);
    }
    ((ULONG (WINAPI *)(void *))vtable[2])(factory);
    return cached;
}

/*
 * Wine does not answer KMTQAITYPE_ADAPTERTYPE, and the framework treats the failure as
 * "not a hardware adapter" and drops the adapter from every -AdapterType subset.  It is
 * answered here, but only where the real call declined: on Windows it succeeds and the
 * fallback never runs.
 */
LONG KMT_API D3DKMTQueryAdapterInfo(void *arg)
{
    static kmt_fn real;
    static BOOL tried;
    kmt_query_adapter_info *q = arg;
    LONG status;

    if (!tried)
    {
        real = resolve("gdi32.dll", "D3DKMTQueryAdapterInfo");
        tried = TRUE;
    }

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
