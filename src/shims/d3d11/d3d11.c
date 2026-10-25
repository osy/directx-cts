/*
 * d3d11 shim: the two things that stop HLK tests being exercised on this platform
 * before any D3D call is made - an unbound import, and a device the framework
 * declines - plus the hand-off to a backend compatibility layer (common/backend.h),
 * which is where everything the host D3D11 implementation gets wrong is handled.
 *
 * The device and its immediate context are wrapped rather than patched in place; see
 * common/comwrap.h for why the real vtable cannot be written.  Every workaround here
 * has a knob (common/knob.h) and is off by default on Windows, where the runtime
 * behaves and applying one would mask a genuine conformance failure.
 */

#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#include "../../common/backend.h"
#include "../../common/comwrap.h"
#include "../../common/d3d11_ifaces.h"
#include "../../common/knob.h"
#include "../../common/shim.h"

#define D3D11_API __declspec(dllexport) WINAPI

#define SLOT  DXCTS_SLOT
#define WIDTH DXCTS_WIDTH

DXCTS_KNOB(patch_device_probe,  "DXCTS_D3D11_DEVICE_PROBE_IFACE",  1, 0)
DXCTS_KNOB(patch_verifier,      "DXCTS_D3D11_LOAD_VERIFIER",       1, 0)
DXCTS_KNOB(patch_pin_crypto,    "DXCTS_D3D11_PIN_CRYPTO",          1, 0)

typedef void (WINAPI *get_context_fn)(void *self, void **context);
typedef HRESULT (WINAPI *create_deferred_fn)(void *self, UINT flags, void **context);
typedef HRESULT (WINAPI *create_from_dxgi_fn)(void *dxgi_device, void **graphics_device);

typedef dxcts_wrapper device_wrapper;

typedef struct
{
    dxcts_wrapper base;
    /* The wrapped device this context belongs to, so ID3D11DeviceChild::GetDevice does
     * not hand back the unpatched one. */
    void *device;
} context_wrapper;

static void *wrap_context(void *device, void *ctx);

static HMODULE g_real_d3d11;

static void *real_proc(const char *name)
{
    return dxcts_real_proc(&g_real_d3d11, "d3d11.dll", name);
}

/*
 * {4BCC5537-3A9C-4573-8D65-8F5B7B15FEF4}, queried on the device by D3DSystemCheck,
 * WGF11Filter, WGF11Draw, WGF11Clipping, WGF11FillRules, WGF11RenderTargets,
 * WGF11ResourceFormats and DXGIMultiPresent.  No Wine or host module contains it, so
 * the query fails and the callers take their unsupported path.
 *
 * In WGF11Filter that path is what stops the modern branch of InitTestGroups from
 * registering anything: QueryDeviceProperties stores the result of a method on this
 * interface at +0x4fb8, and the branch taken when the driver reports WDDM 1.3 or newer
 * tests that field before registering a group.  Answering the query with an object
 * whose methods report zero is what a device that has the interface and nothing to say
 * through it looks like, and both WGF11Filter and WGF11SharedResources then enumerate
 * their groups on the driver version the host actually reports.
 */
static const GUID k_iid_device_probe =
    { 0x4bcc5537, 0x3a9c, 0x4573, { 0x8d, 0x65, 0x8f, 0x5b, 0x7b, 0x15, 0xfe, 0xf4 } };
static const GUID *const k_device_probe_iids[] = { &k_iid_device_probe };

/*
 * The HLK device manager checks whether the D3D driver verifier is present and reports
 * "Driver Verifier DLL not loaded" when it is not, which fails the case whatever the
 * test did.  It never loads the module itself - a +loaddll trace shows no attempt - so
 * on Windows it relies on the D3D runtime having done so.  Nothing does that here, so
 * it is pulled in before the device exists, which is before the manager looks.
 */
static void ensure_verifier_present(void)
{
    static LONG done;

    if (!patch_verifier() || InterlockedCompareExchange(&done, 1, 0) != 0)
        return;
    if (!GetModuleHandleA("D3DDriverVerifier.dll"))
        LoadLibraryA("D3DDriverVerifier.dll");
}

/*
 * The HLK's shader compiler (D3DCompiler_Test.dll) hashes every shader it builds
 * through CryptAcquireContext/CryptReleaseContext, which load and free bcrypt.dll and
 * crypt32.dll each time.  Under Wine the last FreeLibrary unloads the module's unix
 * half, and every load/unload pair leaks dyld state that is never reclaimed: on a
 * large group the process grows to gigabytes of "dyld private memory" and the case
 * rate decays until the group times out, whatever the D3D implementation.  Holding a
 * reference of our own keeps both loaded; deliberately never freed.
 */
static void pin_crypto_modules(void)
{
    static LONG done;

    if (!patch_pin_crypto() || InterlockedCompareExchange(&done, 1, 0) != 0)
        return;
    LoadLibraryA("bcrypt.dll");
    LoadLibraryA("crypt32.dll");
}

/* --- context wrapper --- */


static HRESULT WINAPI ctx_qi(void *self, const GUID *iid, void **out)
{
    context_wrapper *w = self;
    dxcts_qi_fn fn = (dxcts_qi_fn)dxcts_real_vtable(self)[0];

    if (iid && out
        && dxcts_wrapper_answers(&w->base, iid, dxcts_d3d11_contexts,
                           DXCTS_COUNT(dxcts_d3d11_contexts)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return fn(dxcts_real(self), iid, out);
}

/* Wrapping the context opens a path from a wrapped object back to the unpatched device,
 * and a helper that reaches the device that way gets none of the corrections.  It is
 * also an identity mismatch: the framework holds the wrapped device, so GetDevice must
 * return the same pointer. */
static void WINAPI ctx_get_device(void *self, void **device)
{
    context_wrapper *w = self;

    if (device && w->device)
    {
        dxcts_wrapper_addref(w->device);
        *device = w->device;
        return;
    }
    ((get_context_fn)
        dxcts_real_vtable(self)[SLOT(ID3D11DeviceContextVtbl,
                                     GetDevice)])(dxcts_real(self), device);
}

/*
 * One wrapper per real context: the framework holds the immediate context across calls
 * and compares what it gets back, so every entry point that hands one out has to hand
 * out the same object.  There is one immediate context per device and few deferred
 * ones, so a small table is enough; an overflow falls back to the real context, which
 * loses this context's GetDevice rather than breaking the run.
 */
#define MAX_CONTEXTS 16
static context_wrapper *g_contexts[MAX_CONTEXTS];
static LONG g_contexts_lock;

static void ctx_lock(void)
{
    while (InterlockedCompareExchange(&g_contexts_lock, 1, 0))
        Sleep(0);
}

static void ctx_unlock(void)
{
    InterlockedExchange(&g_contexts_lock, 0);
}

/*
 * The whole release runs under the table lock, the decrement included.  Dropping the
 * lock before the decrement leaves a window where another thread's lookup finds the
 * entry, revives it from zero and hands a caller a wrapper this thread then frees.
 */
static ULONG WINAPI ctx_release(void *self)
{
    ULONG refs;
    size_t i;

    ctx_lock();
    if (((dxcts_wrapper *)self)->refs == 1)
        for (i = 0; i < MAX_CONTEXTS; i++)
            if (g_contexts[i] == self)
            {
                g_contexts[i] = NULL;
                break;
            }
    refs = dxcts_wrapper_release(self);
    ctx_unlock();
    return refs;
}

static void *wrap_context(void *device, void *ctx)
{
    static int probed_slots;
    context_wrapper *w;
    int slots;
    size_t i;

    if (!dxcts_wrap_supported())
        return ctx;

    ctx_lock();
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (g_contexts[i] && g_contexts[i]->base.real == ctx)
        {
            w = g_contexts[i];
            dxcts_wrapper_addref(w);
            ctx_unlock();
            /* The wrapper already holds the one reference it needs on the real
             * context, so the reference the caller was just given is surplus. */
            ((dxcts_ref_fn)(*(void ***)ctx)[2])(ctx);
            return w;
        }

    slots = dxcts_widest_slots(ctx, dxcts_d3d11_contexts,
                               DXCTS_COUNT(dxcts_d3d11_contexts), &probed_slots);
    w = (context_wrapper *)dxcts_wrap(ctx, slots, sizeof(*w));
    if (!w)
    {
        ctx_unlock();
        return ctx;
    }
    w->device = device;
    w->base.vtable[0] = (void *)ctx_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)ctx_release;
    DXCTS_INSTALL(&w->base, ID3D11DeviceContextVtbl, GetDevice, ctx_get_device);

    for (i = 0; i < MAX_CONTEXTS; i++)
        if (!g_contexts[i])
        {
            g_contexts[i] = w;
            break;
        }
    if (i == MAX_CONTEXTS)
    {
        /* Unregistered wrappers would break the identity the table exists to keep.
         * The wrapper took over the caller's reference, so one is taken back before
         * discarding it. */
        ctx_unlock();
        ((dxcts_ref_fn)(*(void ***)ctx)[1])(ctx);      /* slot 1 is AddRef */
        dxcts_wrapper_release(w);
        return ctx;
    }
    ctx_unlock();
    return w;
}

/* --- device wrapper --- */


/*
 * Every way a context leaves the device is wrapped.  Covering only
 * GetImmediateContext leaves the same context reachable unwrapped through the
 * later revisions and through the deferred-context calls.
 */
#define DEVICE_GET_CONTEXT(fn, vtbl, method)                                  \
    static void WINAPI fn(void *self, void **context)                         \
    {                                                                         \
        ((get_context_fn)dxcts_real_vtable(self)[SLOT(vtbl, method)])(         \
            dxcts_real(self), context);                                       \
        if (context && *context)                                              \
            *context = wrap_context(self, *context);                          \
    }

DEVICE_GET_CONTEXT(dev_get_context,  ID3D11DeviceVtbl,  GetImmediateContext)
DEVICE_GET_CONTEXT(dev_get_context1, ID3D11Device1Vtbl, GetImmediateContext1)
DEVICE_GET_CONTEXT(dev_get_context2, ID3D11Device2Vtbl, GetImmediateContext2)
DEVICE_GET_CONTEXT(dev_get_context3, ID3D11Device3Vtbl, GetImmediateContext3)

#define DEVICE_CREATE_DEFERRED(fn, vtbl, method)                              \
    static HRESULT WINAPI fn(void *self, UINT flags, void **context)          \
    {                                                                         \
        HRESULT hr = ((create_deferred_fn)                                     \
            dxcts_real_vtable(self)[SLOT(vtbl, method)])(dxcts_real(self),      \
                                                        flags, context);       \
                                                                              \
        if (SUCCEEDED(hr) && context && *context)                             \
            *context = wrap_context(self, *context);                          \
        return hr;                                                            \
    }

DEVICE_CREATE_DEFERRED(dev_deferred,  ID3D11DeviceVtbl,  CreateDeferredContext)
DEVICE_CREATE_DEFERRED(dev_deferred1, ID3D11Device1Vtbl, CreateDeferredContext1)
DEVICE_CREATE_DEFERRED(dev_deferred2, ID3D11Device2Vtbl, CreateDeferredContext2)
DEVICE_CREATE_DEFERRED(dev_deferred3, ID3D11Device3Vtbl, CreateDeferredContext3)

static HRESULT WINAPI dev_qi(void *self, const GUID *iid, void **out)
{
    dxcts_wrapper *w = self;
    dxcts_qi_fn fn = (dxcts_qi_fn)dxcts_real_vtable(self)[0];

    if (!iid || !out)
        return E_POINTER;
    if (patch_device_probe() && IsEqualGUID(iid, &k_iid_device_probe))
    {
        void *probe = dxcts_stub_object(k_device_probe_iids, 1, S_OK);

        if (probe)
        {
            *out = probe;
            return S_OK;
        }
    }
    if (dxcts_wrapper_answers(w, iid, dxcts_d3d11_devices,
                              DXCTS_COUNT(dxcts_d3d11_devices)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return fn(w->real, iid, out);
}

static void *wrap_device(void *device)
{
    static int probed_slots;
    device_wrapper *w;

    if (!dxcts_wrap_supported())
        return device;
    w = dxcts_wrap(device, dxcts_widest_slots(device, dxcts_d3d11_devices,
                                              DXCTS_COUNT(dxcts_d3d11_devices),
                                              &probed_slots),
                   sizeof(*w));
    if (!w)
        return device;
    w->vtable[0] = (void *)dev_qi;
    w->vtable[1] = (void *)dxcts_wrapper_addref;
    w->vtable[2] = (void *)dxcts_wrapper_release;

#define INSTALL(vtbl, method, fn) DXCTS_INSTALL(w, vtbl, method, fn)
    /* Every way a context leaves the device: a context handed out unwrapped is a route
     * back to the unwrapped device through GetDevice. */
    INSTALL(ID3D11DeviceVtbl,  GetImmediateContext,   dev_get_context);
    INSTALL(ID3D11Device1Vtbl, GetImmediateContext1,  dev_get_context1);
    INSTALL(ID3D11Device2Vtbl, GetImmediateContext2,  dev_get_context2);
    INSTALL(ID3D11Device3Vtbl, GetImmediateContext3,  dev_get_context3);
    INSTALL(ID3D11DeviceVtbl,  CreateDeferredContext,  dev_deferred);
    INSTALL(ID3D11Device1Vtbl, CreateDeferredContext1, dev_deferred1);
    INSTALL(ID3D11Device2Vtbl, CreateDeferredContext2, dev_deferred2);
    INSTALL(ID3D11Device3Vtbl, CreateDeferredContext3, dev_deferred3);
#undef INSTALL
    return w;
}

/* Whatever the entry point, the device and its immediate context leave through here, so
 * neither can reach the caller unwrapped. */
static void wrap_created(void **device, void **context)
{
    void *wrapped;

    if (!device || !*device || !patch_device_probe())
        return;
    wrapped = wrap_device(*device);
    if (context && *context)
        /* NULL when the device could not be wrapped: the context must not then treat a
         * raw device as one. */
        *context = wrap_context(wrapped == *device ? NULL : wrapped, *context);
    *device = wrapped;
}

HRESULT D3D11_API dxcts_create_device(void *adapter, UINT driver_type,
                                      HMODULE software, UINT flags,
                                      const void *feature_levels,
                                      UINT num_feature_levels, UINT sdk_version,
                                      void **device, UINT *feature_level,
                                      void **context)
{
    static dxcts_d3d11_create_fn fn;
    static dxcts_backend_d3d11_create_fn backend;
    static int looked_up;
    HRESULT hr;

    if (!fn)
        fn = (dxcts_d3d11_create_fn)real_proc("D3D11CreateDevice");
    if (!fn)
        return E_FAIL;
    if (!looked_up)
    {
        backend = (dxcts_backend_d3d11_create_fn)
            dxcts_backend_proc((const void *)dxcts_create_device,
                               "dxcts_backend_d3d11_create_device");
        looked_up = 1;
    }
    ensure_verifier_present();
    pin_crypto_modules();

    hr = backend ? backend(fn, adapter, driver_type, software, flags, feature_levels,
                           num_feature_levels, sdk_version, device, feature_level,
                           context)
                 : fn(adapter, driver_type, software, flags, feature_levels,
                      num_feature_levels, sdk_version, device, feature_level, context);
    if (SUCCEEDED(hr))
        wrap_created(device, context);
    return hr;
}

/*
 * The same device by another entry point.  Left as a plain forwarder it is a way for a
 * test to obtain a device with none of the corrections above - the swapchain variant is
 * how twelve of the kit's binaries create theirs.
 */
HRESULT D3D11_API dxcts_create_device_and_swapchain(
    void *adapter, UINT driver_type, HMODULE software, UINT flags,
    const void *feature_levels, UINT num_feature_levels, UINT sdk_version,
    const void *swapchain_desc, void **swapchain, void **device, UINT *feature_level,
    void **context)
{
    static dxcts_d3d11_create_swapchain_fn fn;
    static dxcts_backend_d3d11_create_swapchain_fn backend;
    static int looked_up;
    HRESULT hr;

    if (!fn)
        fn = (dxcts_d3d11_create_swapchain_fn)real_proc("D3D11CreateDeviceAndSwapChain");
    if (!fn)
        return E_FAIL;
    if (!looked_up)
    {
        backend = (dxcts_backend_d3d11_create_swapchain_fn)
            dxcts_backend_proc((const void *)dxcts_create_device,
                               "dxcts_backend_d3d11_create_device_and_swapchain");
        looked_up = 1;
    }
    ensure_verifier_present();
    pin_crypto_modules();

    hr = backend ? backend(fn, adapter, driver_type, software, flags, feature_levels,
                           num_feature_levels, sdk_version, swapchain_desc, swapchain,
                           device, feature_level, context)
                 : fn(adapter, driver_type, software, flags, feature_levels,
                      num_feature_levels, sdk_version, swapchain_desc, swapchain, device,
                      feature_level, context);
    if (SUCCEEDED(hr))
        wrap_created(device, context);
    return hr;
}

/*
 * WinRT interop with no Wine equivalent.  Permanent, not a knob: the five DXGI tests
 * that import it cannot load without the export, and failing the call leaves the rest
 * of each test runnable.  Where the platform's own d3d11 has it - Windows, where this
 * shim is staged for the backend hand-off - the call is forwarded.
 */
HRESULT D3D11_API CreateDirect3D11DeviceFromDXGIDevice(void *dxgi_device,
                                                       void **graphics_device)
{
    static create_from_dxgi_fn fn;
    static int looked_up;

    if (!looked_up)
    {
        if (!dxcts_wine())
            fn = (create_from_dxgi_fn)real_proc("CreateDirect3D11DeviceFromDXGIDevice");
        looked_up = 1;
    }
    if (fn)
        return fn(dxgi_device, graphics_device);
    if (graphics_device)
        *graphics_device = NULL;
    return E_NOTIMPL;
}

DXCTS_SHIM_DLLMAIN()
