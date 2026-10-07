/*
 * Backend compatibility layers.
 *
 * The shims beside the tests make the binaries load and answer capability queries the
 * way D3D defines them; they never change what a test measures.  A backend layer is
 * the other kind of shim: one DLL per host D3D implementation, holding the workarounds
 * for what that implementation gets wrong - a create that aborts the process instead
 * of failing, a format it uploads incorrectly, a device it never frees.  Those do
 * change what a test sees, so they are never applied by default: the runner names one
 * with --backend NAME, which reaches the shims as DXCTS_BACKEND=NAME, and the d3d11
 * and d3d12 shims then load dxcts_backend_NAME.dll from beside themselves and route
 * every device creation through it.
 *
 * A layer takes the real entry point rather than the created object, so it can serve
 * a creation some other way (a parked device) as well as wrap what came back.  The
 * layer's wrapper is the innermost one: the shim wraps whatever the layer returns, so
 * the identity the shim's own wrappers keep (a context's GetDevice) is unaffected.
 *
 * The same layer runs on every platform the backend runs on - under Wine, where the
 * implementation is in-process, and on a guest whose driver forwards to it - which is
 * what makes their results comparable.
 */

#ifndef DXCTS_BACKEND_H
#define DXCTS_BACKEND_H

#include <windows.h>
#include <string.h>

typedef HRESULT (WINAPI *dxcts_d3d11_create_fn)(void *adapter, UINT driver_type,
                                                HMODULE software, UINT flags,
                                                const void *feature_levels,
                                                UINT num_feature_levels,
                                                UINT sdk_version, void **device,
                                                UINT *feature_level, void **context);
typedef HRESULT (WINAPI *dxcts_d3d11_create_swapchain_fn)(
    void *adapter, UINT driver_type, HMODULE software, UINT flags,
    const void *feature_levels, UINT num_feature_levels, UINT sdk_version,
    const void *swapchain_desc, void **swapchain, void **device, UINT *feature_level,
    void **context);
typedef HRESULT (WINAPI *dxcts_d3d12_create_fn)(void *adapter, UINT min_feature_level,
                                                const GUID *iid, void **device);

/* The three exports of a backend layer.  Each performs the creation itself through
 * `real` and returns what the caller should see. */
typedef HRESULT (WINAPI *dxcts_backend_d3d11_create_fn)(
    dxcts_d3d11_create_fn real, void *adapter, UINT driver_type, HMODULE software,
    UINT flags, const void *feature_levels, UINT num_feature_levels, UINT sdk_version,
    void **device, UINT *feature_level, void **context);
typedef HRESULT (WINAPI *dxcts_backend_d3d11_create_swapchain_fn)(
    dxcts_d3d11_create_swapchain_fn real, void *adapter, UINT driver_type,
    HMODULE software, UINT flags, const void *feature_levels, UINT num_feature_levels,
    UINT sdk_version, const void *swapchain_desc, void **swapchain, void **device,
    UINT *feature_level, void **context);
typedef HRESULT (WINAPI *dxcts_backend_d3d12_create_fn)(
    dxcts_d3d12_create_fn real, void *adapter, UINT min_feature_level, const GUID *iid,
    void **device);

/* The backend name the runner set, validated to a plain identifier so it can only
 * name a module beside this one.  Empty when no layer was asked for. */
static inline const char *dxcts_backend_name(void)
{
    static char name[32];
    static int looked_up;

    if (!looked_up)
    {
        DWORD len = GetEnvironmentVariableA("DXCTS_BACKEND", name, sizeof(name));
        DWORD i;

        looked_up = 1;
        if (!len || len >= sizeof(name))
            name[0] = 0;
        for (i = 0; name[i]; i++)
            if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9')
                  || name[i] == '_'))
            {
                name[0] = 0;
                break;
            }
    }
    return name;
}

/*
 * The layer's module, loaded once from the directory of the calling shim.  `anchor`
 * is any function in the calling module, which is how its directory is found without
 * knowing its own name.  NULL when no layer is selected or its DLL is absent; the
 * shims then behave as if no layer had been asked for, and the report's `target`
 * records what the runner set so the absence is visible.
 */
static inline HMODULE dxcts_backend_module(const void *anchor)
{
    static HMODULE module;
    static int looked_up;
    const char *name = dxcts_backend_name();
    HMODULE self = NULL;
    char path[MAX_PATH];
    char *slash;
    size_t n;

    if (looked_up)
        return module;
    looked_up = 1;
    if (!name[0])
        return NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)anchor, &self)
        || !GetModuleFileNameA(self, path, MAX_PATH))
        return NULL;
    slash = strrchr(path, '\\');
    n = slash ? (size_t)(slash + 1 - path) : 0;
    if (n + strlen("dxcts_backend_") + strlen(name) + strlen(".dll") >= MAX_PATH)
        return NULL;
    strcpy(path + n, "dxcts_backend_");
    strcat(path, name);
    strcat(path, ".dll");
    module = LoadLibraryA(path);
    return module;
}

static inline void *dxcts_backend_proc(const void *anchor, const char *name)
{
    HMODULE module = dxcts_backend_module(anchor);

    return module ? (void *)GetProcAddress(module, name) : NULL;
}

#endif
