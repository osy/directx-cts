/*
 * dxgi shim: three things a run can ask DXGI enumeration for, none of them on by
 * default.
 *
 * Objects are wrapped rather than patched; see common/comwrap.h.  What each request
 * does and why is documented at the function that implements it.
 */

#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <dxgi1_6.h>

#include "../../common/comwrap.h"
#include "../../common/knob.h"
#include "../../common/shim.h"

#define DXGI_API __declspec(dllexport) WINAPI

#define SLOT  DXCTS_SLOT
#define WIDTH DXCTS_WIDTH

/*
 * These are requests, not knobs: they say what this binary needs in order to be
 * testable at all, and the runner sets them from the catalog.  A knob must never veto
 * one - a request that cannot be honoured has to fail the test rather than quietly
 * leave it running against something it did not ask for, which would report a
 * conformance result for a configuration nobody selected.  Only the runner decides
 * whether to make them.
 */
DXCTS_REQUEST(software_adapter_requested, "DXCTS_SOFTWARE_ADAPTER")
DXCTS_REQUEST(dwm_output_requested, "DXCTS_DWM_OUTPUT")
/* Not a workaround for a defect, so no knob: it is how the software-rasteriser arm is
 * pointed at WARP, and it must work identically on both platforms. */
DXCTS_REQUEST(warp_only, "DXCTS_WARP_ONLY")

static int anything_requested(void)
{
    return software_adapter_requested() || dwm_output_requested() || warp_only();
}

typedef HRESULT (WINAPI *enum_adapters_fn)(void *self, UINT index, void **adapter);
typedef HRESULT (WINAPI *enum_by_preference_fn)(void *self, UINT index, UINT pref,
                                                const GUID *iid, void **adapter);
typedef HRESULT (WINAPI *enum_warp_fn)(void *self, const GUID *iid, void **adapter);
typedef HRESULT (WINAPI *get_desc_fn)(void *self, void *desc);
typedef HRESULT (WINAPI *create_factory_fn)(const GUID *iid, void **out);
typedef HRESULT (WINAPI *create_factory2_fn)(UINT flags, const GUID *iid, void **out);

/* What kind of object a wrapper is over, since the three have different widths and
 * different methods to override. */
typedef enum { KIND_FACTORY, KIND_ADAPTER, KIND_OUTPUT } object_kind;

typedef struct
{
    dxcts_wrapper base;
    object_kind kind;
    /* factory only: index of the adapter to describe as software, or -1 when none
     * should be. */
    int software_index;
} wrapper;

static HMODULE g_real_dxgi;

static void *real_proc(const char *name)
{
    return dxcts_real_proc(&g_real_dxgi, "dxgi.dll", name);
}

static const GUID k_iid_adapter1 =
    { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };

static const dxcts_iface k_factories[] = {
    { { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69 } },
      WIDTH(IDXGIFactoryVtbl) },
    { { 0x770aae78, 0xf26f, 0x4dba, { 0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87 } },
      WIDTH(IDXGIFactory1Vtbl) },
    { { 0x50c83a1c, 0xe072, 0x4c48, { 0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0 } },
      WIDTH(IDXGIFactory2Vtbl) },
    { { 0x25483823, 0xcd46, 0x4c7d, { 0x86,0xca,0x47,0xaa,0x95,0xb8,0x37,0xbd } },
      WIDTH(IDXGIFactory3Vtbl) },
    { { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf,0x0c,0x21,0xca,0x39,0xe5,0x16,0x8a } },
      WIDTH(IDXGIFactory4Vtbl) },
    { { 0x7632e1f5, 0xee65, 0x4dca, { 0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d } },
      WIDTH(IDXGIFactory5Vtbl) },
    { { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0,0x3c,0x77,0x90,0x0a,0x0a,0x1d,0x17 } },
      WIDTH(IDXGIFactory6Vtbl) },
    { { 0xa4966eed, 0x76db, 0x44da, { 0x84,0xc1,0xee,0x9a,0x7a,0xfb,0x20,0xa8 } },
      WIDTH(IDXGIFactory7Vtbl) },
};
static const dxcts_iface k_adapters[] = {
    { { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd,0x14,0x97,0x98,0xe8,0x53,0x4d,0xc0 } },
      WIDTH(IDXGIAdapterVtbl) },
    { { 0x29038f61, 0x3839, 0x4626, { 0x91,0xfd,0x08,0x68,0x79,0x01,0x1a,0x05 } },
      WIDTH(IDXGIAdapter1Vtbl) },
    { { 0x0aa1ae0a, 0xfa0e, 0x4b84, { 0x86,0x44,0xe0,0x5f,0xf8,0xe5,0xac,0xb5 } },
      WIDTH(IDXGIAdapter2Vtbl) },
    { { 0x645967a4, 0x1392, 0x4310, { 0xa7,0x98,0x80,0x53,0xce,0x3e,0x93,0xfd } },
      WIDTH(IDXGIAdapter3Vtbl) },
    { { 0x3c8d99d1, 0x4fbf, 0x4181, { 0xa8,0x2c,0xaf,0x66,0xbf,0x7b,0xd2,0x4e } },
      WIDTH(IDXGIAdapter4Vtbl) },
};
/* Outputs need their own table: sized from the adapter one, an IDXGIOutput6 wrapper
 * would be ten slots short and a call into one of them would jump past its end. */
static const dxcts_iface k_outputs[] = {
    { { 0xae02eedb, 0xc735, 0x4690, { 0x8d,0x52,0x5a,0x8d,0xc2,0x02,0x13,0xaa } },
      WIDTH(IDXGIOutputVtbl) },
    { { 0x00cddea8, 0x939b, 0x4b83, { 0xa3,0x40,0xa6,0x85,0x22,0x66,0x66,0xcc } },
      WIDTH(IDXGIOutput1Vtbl) },
    { { 0x595e39d1, 0x2724, 0x4663, { 0x99,0xb1,0xda,0x96,0x9d,0xe2,0x83,0x64 } },
      WIDTH(IDXGIOutput2Vtbl) },
    { { 0x8a6bb301, 0x7e7e, 0x41F4, { 0xa8,0xe0,0x5b,0x32,0xf7,0xf9,0x9b,0x18 } },
      WIDTH(IDXGIOutput3Vtbl) },
    { { 0xdc7dca35, 0x2196, 0x414d, { 0x9F,0x53,0x61,0x78,0x84,0x03,0x2a,0x60 } },
      WIDTH(IDXGIOutput4Vtbl) },
    { { 0x80A07424, 0xAB52, 0x42EB, { 0x83,0x3C,0x0C,0x42,0xFD,0x28,0x2D,0x98 } },
      WIDTH(IDXGIOutput5Vtbl) },
    { { 0x068346e8, 0xaaec, 0x4b84, { 0xad,0xd7,0x13,0x7f,0x51,0x3f,0x77,0xa1 } },
      WIDTH(IDXGIOutput6Vtbl) },
};

static const dxcts_iface *set_of(object_kind kind, size_t *count)
{
    switch (kind)
    {
    case KIND_FACTORY:
        *count = sizeof(k_factories) / sizeof(k_factories[0]);
        return k_factories;
    case KIND_ADAPTER:
        *count = sizeof(k_adapters) / sizeof(k_adapters[0]);
        return k_adapters;
    default:
        *count = sizeof(k_outputs) / sizeof(k_outputs[0]);
        return k_outputs;
    }
}

/*
 * DXGI_ADAPTER_DESC1, 2 and 3 share their leading layout up to and including Flags,
 * and the software bit is 2 in all three flag enums, so one accessor serves all three
 * GetDesc revisions.  The offset comes from the header because it is not the same on
 * x86 and x64: three SIZE_T members precede it.
 */
_Static_assert(offsetof(DXGI_ADAPTER_DESC1, Flags)
               == offsetof(DXGI_ADAPTER_DESC2, Flags)
               && offsetof(DXGI_ADAPTER_DESC1, Flags)
                  == offsetof(DXGI_ADAPTER_DESC3, Flags),
               "the three adapter descriptors no longer share their Flags offset");
_Static_assert((int)DXGI_ADAPTER_FLAG_SOFTWARE == (int)DXGI_ADAPTER_FLAG3_SOFTWARE,
               "the software bit differs between the flag enumerations");
#define DESC_FLAGS_OFFSET offsetof(DXGI_ADAPTER_DESC1, Flags)

/* A buffer big enough for whichever descriptor a GetDesc revision writes. */
union any_adapter_desc
{
    DXGI_ADAPTER_DESC1 desc1;
    DXGI_ADAPTER_DESC2 desc2;
    DXGI_ADAPTER_DESC3 desc3;
};

static UINT desc_flags(const void *desc)
{
    UINT flags;

    memcpy(&flags, (const unsigned char *)desc + DESC_FLAGS_OFFSET, sizeof(flags));
    return flags;
}

/*
 * IDXGIOutputDWM, a DWM-private interface Wine does not implement, read out of
 * iFlip.dll's own __uuidof constants at the two QueryInterface call sites that fault.
 *
 * iFlip does not check either query: it calls GetMultiplaneOverlayCaps on the null
 * result and takes the process down, so both its groups run no cases at all.  Answering
 * with an object whose methods report failure turns that into a case that runs and
 * fails on its own terms.  The methods are not documented and are not guessed at.
 */
static const GUID k_dwm_adapter_iid =    /* iFlip.dll RVA 0x3bbd0, on the adapter */
    { 0x1ae9fb77, 0x7181, 0x4326, { 0x8c, 0x90, 0x8e, 0xbc, 0x69, 0xf0, 0xae, 0xf8 } };
static const GUID k_dwm_output_iid =     /* RVA 0x39a70, on the output */
    { 0x6f66a9a0, 0xbece, 0x4ee8, { 0xb1, 0x1b, 0x99, 0x0e, 0xb3, 0x8e, 0xd9, 0x76 } };
static const GUID *const k_dwm_iids[] = { &k_dwm_adapter_iid, &k_dwm_output_iid };

static int is_dwm_iid(const GUID *iid)
{
    size_t i;

    for (i = 0; i < sizeof(k_dwm_iids) / sizeof(k_dwm_iids[0]); i++)
        if (IsEqualGUID(iid, k_dwm_iids[i]))
            return 1;
    return 0;
}

static HRESULT WINAPI wrapper_qi(void *self, const GUID *iid, void **out);
static HRESULT WINAPI wrapper_enum_adapters(void *self, UINT index, void **adapter);
static HRESULT WINAPI wrapper_enum_adapters1(void *self, UINT index, void **adapter);
static HRESULT WINAPI wrapper_enum_by_preference(void *self, UINT index, UINT pref,
                                                 const GUID *iid, void **adapter);
static HRESULT WINAPI wrapper_get_desc1(void *self, void *desc);
static HRESULT WINAPI wrapper_get_desc2(void *self, void *desc);
static HRESULT WINAPI wrapper_get_desc3(void *self, void *desc);
static HRESULT WINAPI wrapper_enum_outputs(void *self, UINT index, void **output);
static void *wrap_output(void *output);

static wrapper *wrap_bare(void *object, object_kind kind)
{
    static int cached[3];
    const dxcts_iface *set;
    size_t count;
    wrapper *w;
    int slots;

    set = set_of(kind, &count);
    slots = dxcts_widest_slots(object, set, count, &cached[kind]);
    w = (wrapper *)dxcts_wrap(object, slots, sizeof(*w));
    if (!w)
        return NULL;
    w->base.vtable[0] = (void *)wrapper_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)dxcts_wrapper_release;
    w->kind = kind;
    w->software_index = -1;
    return w;
}

/*
 * Which adapter to describe as software: the first one, unless the host already
 * enumerates one of its own, in which case enumeration is left alone.
 *
 * DxgTaef's scope setup fixture asserts that EnumAdapters1 reports an adapter flagged
 * DXGI_ADAPTER_FLAG_SOFTWARE and fails every method in the binary without running one
 * when it does not - 465 of them in D3DConf_12_Core.dll alone.  Windows always has the
 * Microsoft Basic Render Driver; neither Wine's DXGI nor the host's enumerates one.
 *
 * The flag goes on an adapter that is already there rather than on an appended one.  An
 * extra enumeration slot does satisfy the D3D12 fixture, but it also makes the D3D11
 * device cache take a software-device path this host cannot service, and
 * D3DConf_11_Core then hangs in its scope setup where without it the cases run.
 * Describing a real adapter as software satisfies the fixture with no second adapter
 * and no second device, and a device created on it is the same device as on the
 * hardware adapter, so a test that renders through it is still exercised.
 */
/* DXGI stops enumerating long before this; it exists so a factory that never reports
 * DXGI_ERROR_NOT_FOUND cannot spin here. */
#define MAX_ADAPTERS 64

static int find_software_slot(void *factory, int slots)
{
    void **vtable = *(void ***)factory;
    enum_adapters_fn enumerate;
    UINT index;

    /* EnumAdapters1 is reached through the real vtable, so the object has to be at
     * least IDXGIFactory1: indexing past its end reads whatever follows it in memory. */
    if (SLOT(IDXGIFactory1Vtbl, EnumAdapters1) >= slots)
        return -1;
    enumerate = (enum_adapters_fn)vtable[SLOT(IDXGIFactory1Vtbl, EnumAdapters1)];

    for (index = 0; index < MAX_ADAPTERS; index++)
    {
        union any_adapter_desc desc;
        void *adapter = NULL;
        get_desc_fn get_desc;
        UINT flags = 0;

        if (FAILED(enumerate(factory, index, &adapter)) || !adapter)
            break;
        memset(&desc, 0, sizeof(desc));
        get_desc = (get_desc_fn)
            (*(void ***)adapter)[SLOT(IDXGIAdapter1Vtbl, GetDesc1)];
        if (SUCCEEDED(get_desc(adapter, &desc)))
            flags = desc_flags(&desc);
        ((dxcts_ref_fn)(*(void ***)adapter)[2])(adapter);
        if (flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            return -1;              /* the host has one of its own */
    }
    /* index == 0 means the factory enumerated nothing, so the request cannot be
     * honoured at all.  Nothing is faked in that case: the tests then fail on the
     * absent adapter, which is the truth, rather than running against a device they
     * did not ask for. */
    return index ? 0 : -1;
}

/*
 * Only the first two factories a process creates are shown a software adapter.
 *
 * D3D12ConfSetup builds three device caches.  The first two are the D3D12 ones and
 * assert that a software adapter exists.  The third is the D3D11 cache -
 * DeviceCacheBase<0> - and it is the one that hangs when it finds one, which is the
 * same defect D3DConf_11_Core shows.  Shown one here, twenty D3D12 classes hang instead
 * of running; not shown one, the third cache reports no software adapter (true of this
 * host) and the tests run.  Measured: D3DConf_12_0_ROV#metadataSet0#0 goes from a 100 s
 * timeout with no cases to 5 s with two, one of them passing.
 *
 * The count is what this fixture does today, so a kit update that changes it fails the
 * setup assert loudly rather than silently skipping.  It counts factory creations, so a
 * QueryInterface for a later revision of a factory already counted does not consume one
 * - that would spend both allowances on a single factory and leave the D3D11 cache as
 * the only one shown an adapter it hangs on.
 */
static int software_adapter_wanted(void)
{
    static LONG factories;

    return software_adapter_requested() && InterlockedIncrement(&factories) <= 2;
}

/*
 * `software_index` is the adapter slot to describe as software, or -1 for none.  A
 * factory reached by QueryInterface inherits it from the wrapper it came from: it is
 * the same factory, so it must enumerate the same way.
 */
static void *wrap_factory(void *factory, int inherited_index, int is_new)
{
    wrapper *w = wrap_bare(factory, KIND_FACTORY);

    if (!w)
        return factory;
#define INSTALL(vtbl, method, fn)                                             \
    do {                                                                      \
        int slot = SLOT(vtbl, method);                                         \
        if (slot < w->base.slots)                                                   \
            w->base.vtable[slot] = (void *)(fn);                               \
    } while (0)
    INSTALL(IDXGIFactoryVtbl,  EnumAdapters,  wrapper_enum_adapters);
    INSTALL(IDXGIFactory1Vtbl, EnumAdapters1, wrapper_enum_adapters1);
    /* IDXGIFactory6's enumeration is by index too, so under WARP-only it has to be
     * filtered the same way or the D3D12 suites reach the hardware adapter through it.
     * EnumAdapterByLuid is left alone: a LUID can only come from an adapter this
     * factory already handed out. */
    if (warp_only())
        INSTALL(IDXGIFactory6Vtbl, EnumAdapterByGpuPreference,
                wrapper_enum_by_preference);
#undef INSTALL
    w->software_index = is_new && software_adapter_wanted()
                      ? find_software_slot(factory, w->base.slots)
                      : (is_new ? -1 : inherited_index);
    return w;
}

static void *wrap_adapter(void *adapter, int software)
{
    wrapper *w = wrap_bare(adapter, KIND_ADAPTER);

    if (!w)
        return adapter;
#define INSTALL(vtbl, method, fn)                                             \
    do {                                                                      \
        int slot = SLOT(vtbl, method);                                         \
        if (slot < w->base.slots)                                                   \
            w->base.vtable[slot] = (void *)(fn);                               \
    } while (0)
    /* Only describe it as software when that is what was asked for: an adapter wrapped
     * just to reach its outputs must still describe itself truthfully.  All three
     * GetDesc revisions are overridden, because the fixture may read any of them and
     * one left alone would contradict the others. */
    if (software)
    {
        INSTALL(IDXGIAdapter1Vtbl, GetDesc1, wrapper_get_desc1);
        INSTALL(IDXGIAdapter2Vtbl, GetDesc2, wrapper_get_desc2);
        INSTALL(IDXGIAdapter4Vtbl, GetDesc3, wrapper_get_desc3);
    }
    if (dwm_output_requested())
        INSTALL(IDXGIAdapterVtbl, EnumOutputs, wrapper_enum_outputs);
#undef INSTALL
    return w;
}

static void *wrap_output(void *output)
{
    wrapper *w = wrap_bare(output, KIND_OUTPUT);

    return w ? (void *)w : output;
}

static HRESULT WINAPI wrapper_qi(void *self, const GUID *iid, void **out)
{
    wrapper *w = self;
    dxcts_qi_fn qi = (dxcts_qi_fn)dxcts_real_vtable(self)[0];
    const dxcts_iface *set;
    void *queried = NULL;
    size_t count;
    int width;
    HRESULT hr;

    if (!iid || !out)
        return E_POINTER;
    if (IsEqualGUID(iid, &dxcts_iid_unknown))
    {
        dxcts_wrapper_addref(w);        /* COM identity: the same pointer */
        *out = w;
        return S_OK;
    }
    if (dwm_output_requested() && is_dwm_iid(iid))
    {
        void *stub = dxcts_stub_object(k_dwm_iids,
                                       sizeof(k_dwm_iids) / sizeof(k_dwm_iids[0]),
                                       E_FAIL);

        if (stub)
        {
            *out = stub;
            return S_OK;
        }
    }
    set = set_of(w->kind, &count);
    width = dxcts_iface_slots(iid, set, count);
    /* Not one this shim wraps, or one the real object does not implement: either way
     * the real QueryInterface answers and no wrapper is built. */
    if (!width || width > w->base.slots)
        return qi(dxcts_real(self), iid, out);

    hr = qi(dxcts_real(self), iid, &queried);
    if (FAILED(hr))
        return hr;
    /* A wrapper per interface, owning the reference QueryInterface just returned.  An
     * adapter reached this way is not the one enumeration singled out, so it describes
     * itself truthfully; a factory is the same factory, so it keeps its answer. */
    if (w->kind == KIND_FACTORY)
        *out = wrap_factory(queried, w->software_index, 0);
    else if (w->kind == KIND_ADAPTER)
        *out = wrap_adapter(queried, 0);
    else
        *out = wrap_output(queried);
    return S_OK;
}

/*
 * WARP-only enumeration.
 *
 * The WGF framework picks its device with -Src:WARP, but TAEF has no equivalent:
 * DxgTaef takes the first adapter DXGI enumerates, so on a machine with a GPU the D3D12
 * suites run on that driver whatever was asked for.  Hiding every adapter but WARP is
 * what makes a software-rasteriser arm possible for them, and that arm is what says
 * whether a failure belongs to the implementation under test.
 */
static HRESULT enum_adapter(wrapper *w, int slot, UINT index, void **adapter)
{
    int software;
    HRESULT hr;

    if (warp_only())
    {
        int warp_slot = SLOT(IDXGIFactory4Vtbl, EnumWarpAdapter);

        if (index > 0 || warp_slot >= w->base.slots)
            return DXGI_ERROR_NOT_FOUND;
        hr = ((enum_warp_fn)dxcts_real_vtable(w)[warp_slot])(dxcts_real(w),
                                                             &k_iid_adapter1,
                                                             adapter);
    }
    else
    {
        hr = ((enum_adapters_fn)dxcts_real_vtable(w)[slot])(dxcts_real(w), index,
                                                            adapter);
    }
    if (FAILED(hr) || !adapter || !*adapter)
        return hr;

    /* The other two requests apply to whichever adapter enumeration reported, WARP
     * included: a run that asks for both must not have one silently dropped because
     * the other changed where the adapter came from. */
    software = w->software_index >= 0 && (int)index == w->software_index;
    if (software || dwm_output_requested())
        *adapter = wrap_adapter(*adapter, software);
    return hr;
}

static HRESULT WINAPI wrapper_enum_adapters(void *self, UINT index, void **adapter)
{
    return enum_adapter(self, SLOT(IDXGIFactoryVtbl, EnumAdapters), index, adapter);
}

static HRESULT WINAPI wrapper_enum_adapters1(void *self, UINT index, void **adapter)
{
    return enum_adapter(self, SLOT(IDXGIFactory1Vtbl, EnumAdapters1), index, adapter);
}

/* Installed only under WARP-only, so the caller's GpuPreference is irrelevant: there
 * is one adapter to report and it is WARP. */
static HRESULT WINAPI wrapper_enum_by_preference(void *self, UINT index, UINT pref,
                                                 const GUID *iid, void **adapter)
{
    wrapper *w = self;
    int warp_slot = SLOT(IDXGIFactory4Vtbl, EnumWarpAdapter);

    (void)pref;
    if (index > 0 || warp_slot >= w->base.slots)
        return DXGI_ERROR_NOT_FOUND;
    return ((enum_warp_fn)dxcts_real_vtable(w)[warp_slot])(dxcts_real(w), iid,
                                                           adapter);
}

static HRESULT WINAPI wrapper_enum_outputs(void *self, UINT index, void **output)
{
    HRESULT hr = ((enum_adapters_fn)
        dxcts_real_vtable(self)[SLOT(IDXGIAdapterVtbl, EnumOutputs)])(dxcts_real(self),
                                                                     index, output);

    if (SUCCEEDED(hr) && output && *output)
        *output = wrap_output(*output);
    return hr;
}

static HRESULT get_desc_as_software(void *self, int slot, void *desc)
{
    HRESULT hr = ((get_desc_fn)dxcts_real_vtable(self)[slot])(dxcts_real(self), desc);

    if (SUCCEEDED(hr) && desc)
    {
        UINT flags = desc_flags(desc) | DXGI_ADAPTER_FLAG_SOFTWARE;

        memcpy((unsigned char *)desc + DESC_FLAGS_OFFSET, &flags, sizeof(flags));
    }
    return hr;
}

static HRESULT WINAPI wrapper_get_desc1(void *self, void *desc)
{
    return get_desc_as_software(self, SLOT(IDXGIAdapter1Vtbl, GetDesc1), desc);
}

static HRESULT WINAPI wrapper_get_desc2(void *self, void *desc)
{
    return get_desc_as_software(self, SLOT(IDXGIAdapter2Vtbl, GetDesc2), desc);
}

static HRESULT WINAPI wrapper_get_desc3(void *self, void *desc)
{
    return get_desc_as_software(self, SLOT(IDXGIAdapter4Vtbl, GetDesc3), desc);
}

static HRESULT wrap_created_factory(HRESULT hr, const GUID *iid, void **out)
{
    if (SUCCEEDED(hr) && out && *out && anything_requested()
        && dxcts_wrap_supported()
        && dxcts_iface_slots(iid, k_factories,
                             sizeof(k_factories) / sizeof(k_factories[0])))
        *out = wrap_factory(*out, -1, 1);
    return hr;
}

HRESULT DXGI_API dxcts_create_factory(const GUID *iid, void **out)
{
    static create_factory_fn fn;

    if (!fn)
        fn = (create_factory_fn)real_proc("CreateDXGIFactory");
    if (!fn)
        return E_FAIL;
    return wrap_created_factory(fn(iid, out), iid, out);
}

HRESULT DXGI_API dxcts_create_factory1(const GUID *iid, void **out)
{
    static create_factory_fn fn;

    if (!fn)
        fn = (create_factory_fn)real_proc("CreateDXGIFactory1");
    if (!fn)
        return E_FAIL;
    return wrap_created_factory(fn(iid, out), iid, out);
}

HRESULT DXGI_API dxcts_create_factory2(UINT flags, const GUID *iid, void **out)
{
    static create_factory2_fn fn;

    if (!fn)
        fn = (create_factory2_fn)real_proc("CreateDXGIFactory2");
    if (!fn)
        return E_FAIL;
    return wrap_created_factory(fn(flags, iid, out), iid, out);
}

DXCTS_SHIM_DLLMAIN()
