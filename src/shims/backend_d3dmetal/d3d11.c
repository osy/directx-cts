/*
 * The D3D11 device and context wrappers, the device-level hooks, and the layer's
 * D3D11 entry points.
 */

#include "d3d11_layer.h"

/* A lock over the layer's tables, kept simple: none of them is on a hot path. */
static SRWLOCK g_lock = SRWLOCK_INIT;

void lock(void)
{
    AcquireSRWLockExclusive(&g_lock);
}

void unlock(void)
{
    ReleaseSRWLockExclusive(&g_lock);
}

/* --- context wrapper --- */

static HRESULT WINAPI ctx_qi(void *self, const GUID *iid, void **out)
{
    dm_context *w = self;

    if (iid && out && dxcts_wrapper_answers(&w->base, iid, dxcts_d3d11_contexts,
                                            DXCTS_COUNT(dxcts_d3d11_contexts)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return ((dxcts_qi_fn)dxcts_real_vtable(self)[0])(dxcts_real(self), iid, out);
}

/* The framework holds the wrapped device, so GetDevice must return the same pointer -
 * and a helper that reached the real device that way would escape every hook. */
static void WINAPI ctx_get_device(void *self, void **device)
{
    dm_context *w = self;

    if (device)
    {
        dxcts_wrapper_addref(w->device);
        *device = w->device;
    }
}

/*
 * One wrapper per real context: the framework holds the immediate context across calls
 * and compares what it gets back, so every entry point that hands one out has to hand
 * out the same object.  An overflow falls back to the real context, which loses this
 * context's guards rather than breaking the run.  The whole release runs under the
 * lock, the decrement included, so a lookup cannot revive an entry being freed.
 */
#define MAX_CONTEXTS 16
static dm_context *g_contexts[MAX_CONTEXTS];

static ULONG WINAPI ctx_release(void *self)
{
    dm_context *w = self;
    dm_device *device = w->device;
    ID3D11BlendState *twin = w->mask_twin;
    ULONG refs;
    size_t i;

    lock();
    if (w->base.refs == 1)
        for (i = 0; i < MAX_CONTEXTS; i++)
            if (g_contexts[i] == w)
            {
                g_contexts[i] = NULL;
                break;
            }
    refs = dxcts_wrapper_release(self);
    unlock();
    if (refs == 0)
    {
        dxcts_release(twin);
        dxcts_release(device);
    }
    return refs;
}

static void *wrap_context(dm_device *device, void *ctx)
{
    static int probed_slots;
    dm_context *w;
    int slots;
    size_t i;

    lock();
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (g_contexts[i] && g_contexts[i]->base.real == ctx)
        {
            w = g_contexts[i];
            dxcts_wrapper_addref(w);
            unlock();
            /* The wrapper already holds the one reference it needs on the real
             * context, so the reference the caller was just given is surplus. */
            dxcts_release(ctx);
            return w;
        }
    slots = dxcts_widest_slots(ctx, dxcts_d3d11_contexts, DXCTS_COUNT(dxcts_d3d11_contexts),
                               &probed_slots);
    w = (dm_context *)dxcts_wrap(ctx, slots, sizeof(*w));
    if (!w)
    {
        unlock();
        return ctx;
    }
    w->device = device;
    dxcts_wrapper_addref(device);
    w->ib_format = DXGI_FORMAT_UNKNOWN;
    w->factor[0] = w->factor[1] = w->factor[2] = w->factor[3] = 1.f;
    w->sample_mask = 0xffffffffu;
    w->base.vtable[0] = (void *)ctx_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)ctx_release;
#define INSTALL(vtbl, method, fn) DXCTS_INSTALL(&w->base, vtbl, method, fn)
    INSTALL(ID3D11DeviceContextVtbl, GetDevice, ctx_get_device);
    INSTALL(ID3D11DeviceContextVtbl, VSSetShader, ctx_vs_set_shader);
    INSTALL(ID3D11DeviceContextVtbl, GSSetShader, ctx_gs_set_shader);
    INSTALL(ID3D11DeviceContextVtbl, HSSetShader, ctx_hs_set_shader);
    INSTALL(ID3D11DeviceContextVtbl, DSSetShader, ctx_ds_set_shader);
    INSTALL(ID3D11DeviceContextVtbl, IASetPrimitiveTopology, ctx_ia_set_topology);
    INSTALL(ID3D11DeviceContextVtbl, IASetInputLayout, ctx_ia_set_layout);
    INSTALL(ID3D11DeviceContextVtbl, IASetVertexBuffers, ctx_ia_set_vbs);
    INSTALL(ID3D11DeviceContextVtbl, IASetIndexBuffer, ctx_ia_set_ib);
    INSTALL(ID3D11DeviceContextVtbl, OMSetBlendState, ctx_om_set_blend);
    INSTALL(ID3D11DeviceContextVtbl, OMSetRenderTargets, ctx_om_set_rts);
    INSTALL(ID3D11DeviceContextVtbl, OMSetRenderTargetsAndUnorderedAccessViews,
            ctx_om_set_rts_uav);
    INSTALL(ID3D11DeviceContextVtbl, ClearState, ctx_clear_state);
    INSTALL(ID3D11DeviceContextVtbl, ExecuteCommandList, ctx_execute_command_list);
    INSTALL(ID3D11DeviceContextVtbl, FinishCommandList, ctx_finish_command_list);
    INSTALL(ID3D11DeviceContext1Vtbl, SwapDeviceContextState, ctx_swap_state);
    INSTALL(ID3D11DeviceContextVtbl, CopySubresourceRegion, ctx_copy_region);
    INSTALL(ID3D11DeviceContextVtbl, CopyResource, ctx_copy_resource);
    INSTALL(ID3D11DeviceContext1Vtbl, CopySubresourceRegion1, ctx_copy_region1);
    INSTALL(ID3D11DeviceContextVtbl, Map, ctx_map);
    INSTALL(ID3D11DeviceContextVtbl, Unmap, ctx_unmap);
    INSTALL(ID3D11DeviceContextVtbl, UpdateSubresource, ctx_update);
    INSTALL(ID3D11DeviceContext1Vtbl, UpdateSubresource1, ctx_update1);
    INSTALL(ID3D11DeviceContextVtbl, ResolveSubresource, ctx_resolve);
    INSTALL(ID3D11DeviceContext1Vtbl, DiscardResource, ctx_discard_resource);
    INSTALL(ID3D11DeviceContextVtbl, SetResourceMinLOD, ctx_set_min_lod);
    INSTALL(ID3D11DeviceContextVtbl, GetResourceMinLOD, ctx_get_min_lod);
    INSTALL(ID3D11DeviceContextVtbl, ClearRenderTargetView, ctx_clear_rtv);
    INSTALL(ID3D11DeviceContextVtbl, Draw, ctx_draw);
    INSTALL(ID3D11DeviceContextVtbl, DrawIndexed, ctx_draw_indexed);
    INSTALL(ID3D11DeviceContextVtbl, DrawInstanced, ctx_draw_instanced);
    INSTALL(ID3D11DeviceContextVtbl, DrawIndexedInstanced, ctx_draw_indexed_instanced);
    INSTALL(ID3D11DeviceContextVtbl, DrawAuto, ctx_draw_auto);
    INSTALL(ID3D11DeviceContextVtbl, DrawInstancedIndirect, ctx_draw_instanced_indirect);
    INSTALL(ID3D11DeviceContextVtbl, DrawIndexedInstancedIndirect,
            ctx_draw_indexed_instanced_indirect);
#undef INSTALL
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (!g_contexts[i])
        {
            g_contexts[i] = w;
            break;
        }
    if (i == MAX_CONTEXTS)
    {
        unlock();
        /* the wrapper took over the caller's reference; take it back */
        dxcts_addref(ctx);
        dxcts_release(device);
        dxcts_wrapper_release(w);
        return ctx;
    }
    unlock();
    return w;
}

/* --- device hooks: contexts --- */

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

/* --- device hooks: layouts and shaders --- */

/* D3DMetal's input layout holds 31 elements - one short of the 32 D3D11 allows - and
 * a 32nd element reads a NULL name off the end of its table and dies in strdup.  It
 * also strdup()s every element's SemanticName unchecked, so a NULL name (which the API
 * forbids) dies the same way. */
#define MAX_INPUT_ELEMENTS 31

/* Bytes of one element of a vertex format (the formats an input layout may name), 0
 * for anything else. */
static UINT vertex_format_bytes(DXGI_FORMAT f)
{
    if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return 16;
    if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return 12;
    if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_R32G32_SINT) return 8;
    if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_R32_SINT) return 4;
    if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return 2;
    if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return 1;
    if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return 4;
    if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM
        || f == DXGI_FORMAT_B4G4R4A4_UNORM) return 2;
    return 0;
}

/* Record the elements on a freshly made layout; D3D11_APPEND_ALIGNED_ELEMENT offsets
 * are resolved the way the runtime does (4-byte aligned after the previous element of
 * the same slot). */
static void record_layout_elements(void *il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n)
{
    layout_elem out[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT next[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = { 0 };
    UINT i;

    if (n > D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        return;
    for (i = 0; i < n; i++)
    {
        UINT slot = e[i].InputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        UINT off = e[i].AlignedByteOffset, size = vertex_format_bytes(e[i].Format);

        if (off == D3D11_APPEND_ALIGNED_ELEMENT)
            off = (next[slot] + 3) & ~3u;
        next[slot] = off + size;
        out[i].slot = slot;
        out[i].offset = off;
        out[i].size = size;
        out[i].per_instance = e[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
        out[i].step = e[i].InstanceDataStepRate;
    }
    ID3D11DeviceChild_SetPrivateData((ID3D11DeviceChild *)il, &k_layout_tag,
                                     n * sizeof(layout_elem), out);
}

static HRESULT WINAPI dev_create_layout(void *self, const D3D11_INPUT_ELEMENT_DESC *elems,
                                        UINT n, const void *code, SIZE_T code_len,
                                        void **out)
{
    create_layout_fn real = (create_layout_fn)REAL_DEV(self, CreateInputLayout);
    HRESULT hr;
    UINT i;

    if (dm_knob_create_guards())
    {
        int bad = n > MAX_INPUT_ELEMENTS;

        for (i = 0; elems && i < n && !bad; i++)
            bad = !elems[i].SemanticName;
        if (bad)
        {
            return dm_refuse(out);
        }
    }
    hr = real(dxcts_real(self), elems, n, code, code_len, out);
    if (SUCCEEDED(hr) && out && *out && elems && n)
        record_layout_elements(*out, elems, n);
    return hr;
}

/* D3DMetal's stream-output declaration table holds 53 entries; the 54th (of the 64
 * D3D allows) overruns it and the process dies inside or right after the create.  An
 * entry with no SemanticName is a gap (the API's way to skip components), which
 * D3DMetal strdup()s and dies on, and given a placeholder name instead fails to build
 * the stream-out function; so a gap cannot be served and the declaration is refused. */
static HRESULT WINAPI dev_create_gs_so(void *self, const void *code, SIZE_T code_len,
                                       const D3D11_SO_DECLARATION_ENTRY *decl, UINT n,
                                       const UINT *strides, UINT n_strides, UINT rasterized,
                                       void *linkage, void **out)
{
    create_gs_so_fn real = (create_gs_so_fn)REAL_DEV(self, CreateGeometryShaderWithStreamOutput);
    UINT i;

    if (dm_knob_create_guards())
    {
        int bad = n > DM_MAX_STREAM_OUTPUT_ENTRIES;

        for (i = 0; decl && i < n && !bad; i++)
            bad = !decl[i].SemanticName;
        if (bad)
        {
            return dm_refuse(out);
        }
    }
    return real(dxcts_real(self), code, code_len, decl, n, strides, n_strides, rasterized,
                linkage, out);
}

static HRESULT WINAPI dev_create_gs(void *self, const void *code, SIZE_T code_len,
                                    void *linkage, void **out)
{
    create_shader_fn real = (create_shader_fn)REAL_DEV(self, CreateGeometryShader);
    HRESULT hr = real(dxcts_real(self), code, code_len, linkage, out);

    if (SUCCEEDED(hr) && out && *out && gs_is_pass_through(code, code_len))
    {
        const unsigned char tag = 1;

        ID3D11DeviceChild_SetPrivateData((ID3D11DeviceChild *)*out, &k_passthrough_tag,
                                         sizeof(tag), &tag);
    }
    return hr;
}

static HRESULT WINAPI dev_create_vs(void *self, const void *code, SIZE_T code_len,
                                    void *linkage, void **out)
{
    create_shader_fn real = (create_shader_fn)REAL_DEV(self, CreateVertexShader);
    HRESULT hr = real(dxcts_real(self), code, code_len, linkage, out);

    if (SUCCEEDED(hr) && out && *out)
    {
        UINT bytes = vertex_output_bytes(code, code_len);

        if (bytes)
            ID3D11DeviceChild_SetPrivateData((ID3D11DeviceChild *)*out, &k_vs_bytes_tag,
                                             sizeof(bytes), &bytes);
    }
    return hr;
}

static HRESULT WINAPI dev_create_ps(void *self, const void *code, SIZE_T code_len,
                                    void *linkage, void **out)
{
    create_shader_fn real = (create_shader_fn)REAL_DEV(self, CreatePixelShader);

    if (dm_knob_minprec_position() && ps_minprec_on_position(code, code_len))
    {
        return dm_refuse(out);
    }
    return real(dxcts_real(self), code, code_len, linkage, out);
}

/* --- device hooks: capabilities --- */

/*
 * DXGI permits either answer for an unsupported sample count: "this method returns a
 * failure HRESULT code, or sets pNumQualityLevels to zero, or both".  The host returns
 * E_INVALIDARG for counts 8 and 16, and the HLK's SetupTestCase treats a failure
 * HRESULT as fatal, so whole groups never reach a draw.  Normalising to the S_OK plus
 * zero-levels form the content does tolerate says the same thing about the device.
 * And the host answers one level for any count on any format it knows, BC and video
 * formats included, so a caller that trusts it goes on to create a multisampled
 * texture Metal then aborts on: a pair Metal cannot carry is answered with zero
 * levels; a pair it can keeps the host's count.
 */
static HRESULT repair_msaa_levels(HRESULT hr, UINT format, UINT sample_count,
                                  UINT *quality_levels)
{
    if (!dm_knob_msaa_levels())
        return hr;
    if (hr == E_INVALIDARG && sample_count >= 1
        && sample_count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT)
    {
        if (quality_levels)
            *quality_levels = 0;
        hr = S_OK;
    }
    if (SUCCEEDED(hr) && quality_levels && !dm_format_msaa_ok(format, sample_count))
        *quality_levels = 0;
    return hr;
}

static HRESULT WINAPI dev_check_msaa(void *self, UINT format, UINT sample_count,
                                     UINT *quality_levels)
{
    check_msaa_fn fn = (check_msaa_fn)REAL_DEV(self, CheckMultisampleQualityLevels);

    return repair_msaa_levels(fn(dxcts_real(self), format, sample_count, quality_levels),
                              format, sample_count, quality_levels);
}

static HRESULT WINAPI dev_check_msaa1(void *self, UINT format, UINT sample_count, UINT flags,
                                      UINT *quality_levels)
{
    check_msaa1_fn fn = (check_msaa1_fn)
        dxcts_real_vtable(self)[SLOT(ID3D11Device2Vtbl, CheckMultisampleQualityLevels1)];

    return repair_msaa_levels(fn(dxcts_real(self), format, sample_count, flags,
                                 quality_levels), format, sample_count, quality_levels);
}

/* The last D3D11 feature the mingw headers know is D3D11_FEATURE_D3D11_OPTIONS5 (19);
 * DISPLAYABLE (20) and its data struct are only in the Windows SDK. */
#ifndef D3D11_FEATURE_DISPLAYABLE
#define D3D11_FEATURE_DISPLAYABLE 20
typedef struct
{
    BOOL DisplayableTexture;
    D3D11_SHARED_RESOURCE_TIER SharedResourceTier;
} D3D11_FEATURE_DATA_DISPLAYABLE;
#endif

static UINT feature_data_size(UINT feature)
{
    switch (feature)
    {
    case D3D11_FEATURE_THREADING:   return sizeof(D3D11_FEATURE_DATA_THREADING);
    case D3D11_FEATURE_DOUBLES:     return sizeof(D3D11_FEATURE_DATA_DOUBLES);
    case D3D11_FEATURE_FORMAT_SUPPORT: return sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT);
    case D3D11_FEATURE_FORMAT_SUPPORT2: return sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT2);
    case D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS:
        return sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS);
    case D3D11_FEATURE_D3D11_OPTIONS: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS);
    case D3D11_FEATURE_ARCHITECTURE_INFO:
        return sizeof(D3D11_FEATURE_DATA_ARCHITECTURE_INFO);
    case D3D11_FEATURE_D3D9_OPTIONS: return sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS);
    case D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT:
        return sizeof(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT);
    case D3D11_FEATURE_D3D9_SHADOW_SUPPORT:
        return sizeof(D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT);
    case D3D11_FEATURE_D3D11_OPTIONS1: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1);
    case D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT:
        return sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT);
    case D3D11_FEATURE_MARKER_SUPPORT: return sizeof(D3D11_FEATURE_DATA_MARKER_SUPPORT);
    case D3D11_FEATURE_D3D9_OPTIONS1: return sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1);
    case D3D11_FEATURE_D3D11_OPTIONS2: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2);
    case D3D11_FEATURE_D3D11_OPTIONS3: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3);
    case D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT:
        return sizeof(D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
    case D3D11_FEATURE_D3D11_OPTIONS4: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS4);
    case D3D11_FEATURE_SHADER_CACHE: return sizeof(D3D11_FEATURE_DATA_SHADER_CACHE);
    case D3D11_FEATURE_D3D11_OPTIONS5: return sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS5);
    case D3D11_FEATURE_DISPLAYABLE: return sizeof(D3D11_FEATURE_DATA_DISPLAYABLE);
    default: return 0;
    }
}

/*
 * A driver that does not implement a feature must answer S_OK and report it
 * unsupported; E_INVALIDARG is for a malformed request.  The host returns E_INVALIDARG
 * for features it does not recognise, and several tests query one from
 * InitTestGroups, so the binary abandons setup before enumerating any case.  Only the
 * size the feature actually takes is repaired: a case that negative-tests a
 * wrong-sized query must still see it fail.  D3D11_FEATURE_FORMAT_SUPPORT "returns
 * the same information that would be returned by CheckFormatSupport", so it gets the
 * same correction.
 */
static HRESULT WINAPI dev_check_feature(void *self, UINT feature, void *data, UINT size)
{
    check_feature_fn fn = (check_feature_fn)REAL_DEV(self, CheckFeatureSupport);
    HRESULT hr = fn(dxcts_real(self), feature, data, size);
    UINT expected = feature_data_size(feature);

    if (SUCCEEDED(hr) && dm_knob_format_support() && feature == D3D11_FEATURE_FORMAT_SUPPORT
        && data && size == expected)
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT *fs = data;

        fs->OutFormatSupport &= ~dm_format_impossible_support(fs->InFormat);
    }
    if (hr == E_INVALIDARG && dm_knob_feature_support() && data && expected && size == expected)
    {
        memset(data, 0, size);
        return S_OK;
    }
    return hr;
}

/* The host does not implement this: it answers 0xffffffff for every format.  Clearing
 * the bits the format cannot have is what a conformant implementation reports; bits
 * that merely might not be supported are left as the host answered them. */
static HRESULT WINAPI dev_check_format(void *self, UINT format, UINT *support)
{
    check_format_fn fn = (check_format_fn)REAL_DEV(self, CheckFormatSupport);
    HRESULT hr = fn(dxcts_real(self), format, support);

    if (SUCCEEDED(hr) && support && dm_knob_format_support())
        *support &= ~dm_format_impossible_support(format);
    return hr;
}

/* --- IDXGIDevice2::EnqueueSetEvent --- */
/*
 * "Flush any outstanding rendering commands and set the event once they complete"
 * returns E_NOTIMPL on this host.  The framework's BlockingFinish() waits on it, so a
 * test that creates a resource and waits for the GPU fails before it tests anything.
 * The contract is exactly a fence signal, and the fence path does work here: flush,
 * signal a fence from the immediate context, and hand the event to
 * SetEventOnCompletion.
 */
static HRESULT WINAPI dxgi_enqueue_set_event(void *self, HANDLE event)
{
    void *real = dxcts_real(self);
    ID3D11Device *dev = NULL;
    ID3D11Device5 *dev5 = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11DeviceContext4 *ctx4 = NULL;
    ID3D11Fence *fence = NULL;
    HRESULT hr;

    hr = ((enqueue_fn)dxcts_real_vtable(self)[SLOT(IDXGIDevice2Vtbl, EnqueueSetEvent)])(real,
                                                                                    event);
    if (hr != E_NOTIMPL)
        return hr;
    if (FAILED(((dxcts_qi_fn)(*(void ***)real)[0])(real, &IID_ID3D11Device, (void **)&dev))
        || !dev)
        return E_NOTIMPL;
    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&dev5);
    if (SUCCEEDED(hr) && dev5)
    {
        ID3D11Device_GetImmediateContext(dev, &ctx);
        if (ctx)
            ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4,
                                               (void **)&ctx4);
        if (ctx4)
            hr = ID3D11Device5_CreateFence(dev5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence,
                                           (void **)&fence);
        if (fence && ctx4)
        {
            ID3D11DeviceContext_Flush(ctx);
            hr = ID3D11DeviceContext4_Signal(ctx4, fence, 1);
            if (SUCCEEDED(hr))
                hr = ID3D11Fence_SetEventOnCompletion(fence, 1, event);
        }
        else if (SUCCEEDED(hr))
            hr = E_NOTIMPL;
    }
    dxcts_release(fence); dxcts_release(ctx4); dxcts_release(ctx); dxcts_release(dev5);
    dxcts_release(dev);
    return hr;
}

static void *wrap_dxgi_device(void *real)
{
    static const dxcts_iface k_dxgi_devices[] = {
        { &IID_IDXGIDevice, WIDTH(IDXGIDeviceVtbl) },
        { &IID_IDXGIDevice1, WIDTH(IDXGIDevice1Vtbl) },
        { &IID_IDXGIDevice2, WIDTH(IDXGIDevice2Vtbl) },
        { &IID_IDXGIDevice3, WIDTH(IDXGIDevice3Vtbl) },
        { &IID_IDXGIDevice4, WIDTH(IDXGIDevice4Vtbl) },
    };
    static int probed_slots;
    dxcts_wrapper *w = dxcts_wrap(real, dxcts_widest_slots(real, k_dxgi_devices,
                                                           DXCTS_COUNT(k_dxgi_devices),
                                                           &probed_slots),
                                  sizeof(*w));

    if (!w)
        return real;
    w->vtable[1] = (void *)dxcts_wrapper_addref;
    w->vtable[2] = (void *)dxcts_wrapper_release;
    DXCTS_INSTALL(w, IDXGIDevice2Vtbl, EnqueueSetEvent, dxgi_enqueue_set_event);
    return w;
}

/* --- device wrapper --- */

static HRESULT WINAPI dev_qi(void *self, const GUID *iid, void **out)
{
    dxcts_wrapper *w = self;
    dxcts_qi_fn fn = (dxcts_qi_fn)dxcts_real_vtable(self)[0];

    if (!iid || !out)
        return E_POINTER;
    if (dxcts_wrapper_answers(w, iid, dxcts_d3d11_devices,
                              DXCTS_COUNT(dxcts_d3d11_devices)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    if (dm_knob_enqueue_set_event()
        && (IsEqualGUID(iid, &IID_IDXGIDevice2) || IsEqualGUID(iid, &IID_IDXGIDevice3)
            || IsEqualGUID(iid, &IID_IDXGIDevice4)))
    {
        HRESULT hr = fn(w->real, iid, out);

        if (SUCCEEDED(hr) && *out)
            *out = wrap_dxgi_device(*out);
        return hr;
    }
    return fn(w->real, iid, out);
}

/* --- device reuse --- */
/*
 * D3DMetal never frees a D3D11 device: the final Release returns 0 and leaves the
 * object, its command queue and its 256 MiB resource pool heap allocated (measured
 * +257 MiB of MTLDevice.currentAllocatedSize per create/release cycle, with and
 * without any shim, and the same growth under CrossOver).  A process that creates a
 * device per test case - the HLK - grows by that much per case until the GPU runs out
 * of memory.  The released object is intact (draws, clears and readbacks keep working
 * after AddRef brings it back), so a device whose reference count reaches zero is
 * parked, its real device never released, and the next D3D11CreateDevice with the same
 * adapter, driver type, flags, feature levels and SDK version gets it back: the
 * immediate context with its state cleared, and the feature level it was created with.
 * Behind a guest driver the same parking keeps the host device alive and reused.
 */
#define MAX_PARKED 8
static dm_device *g_parked[MAX_PARKED];

static int dev_key_of(void *adapter, UINT driver_type, HMODULE software, UINT flags,
                      const void *levels, UINT nlevels, UINT sdk, dev_key *out)
{
    memset(out, 0, sizeof(*out));
    if (software || nlevels > 16)
        return 0;
    if (adapter)
    {
        IDXGIAdapter *ad = NULL;
        DXGI_ADAPTER_DESC desc;

        if (FAILED(((dxcts_qi_fn)(*(void ***)adapter)[0])(adapter, &IID_IDXGIAdapter,
                                                          (void **)&ad)) || !ad)
            return 0;
        if (FAILED(IDXGIAdapter_GetDesc(ad, &desc)))
        {
            dxcts_release(ad);
            return 0;
        }
        dxcts_release(ad);
        out->luid = ((uint64_t)(UINT)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
    }
    out->driver_type = driver_type;
    out->flags = flags;
    out->sdk = sdk;
    out->nlevels = nlevels;
    if (levels && nlevels)
        memcpy(out->levels, levels, nlevels * sizeof(UINT));
    return 1;
}

static ULONG WINAPI dev_release(void *self)
{
    dm_device *w = self;
    LONG refs = InterlockedDecrement(&w->base.refs);
    size_t i;

    if (refs != 0)
        return (ULONG)refs;
    if (w->reusable && dm_knob_device_reuse())
    {
        lock();
        for (i = 0; i < MAX_PARKED; i++)
            if (!g_parked[i])
            {
                g_parked[i] = w;
                break;
            }
        unlock();
        if (i < MAX_PARKED)
            return 0;
    }
    dxcts_release(w->st_vs); dxcts_release(w->st_il); dxcts_release(w->st_dss);
    dxcts_release(w->st_rs);
    dxcts_release(w->base.real);
    dxcts_wrapper_free(&w->base);
    return 0;
}

static dm_device *take_parked(const dev_key *key)
{
    dm_device *w = NULL;
    size_t i;

    lock();
    for (i = 0; i < MAX_PARKED; i++)
        if (g_parked[i] && memcmp(&g_parked[i]->key, key, sizeof(*key)) == 0)
        {
            w = g_parked[i];
            g_parked[i] = NULL;
            break;
        }
    unlock();
    return w;
}

static void *wrap_device(void *device, const dev_key *key, int reusable, UINT level)
{
    static int probed_slots;
    dm_device *w = (dm_device *)dxcts_wrap(device,
                                           dxcts_widest_slots(device, dxcts_d3d11_devices,
                                                              DXCTS_COUNT(dxcts_d3d11_devices),
                                                              &probed_slots),
                                           sizeof(*w));

    if (!w)
        return device;
    if (key)
        w->key = *key;
    w->reusable = reusable;
    w->level = level;
    w->context_thread = GetCurrentThreadId();
    w->base.vtable[0] = (void *)dev_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)dev_release;
#define INSTALL(vtbl, method, fn) DXCTS_INSTALL(&w->base, vtbl, method, fn)
    INSTALL(ID3D11DeviceVtbl,  GetImmediateContext,   dev_get_context);
    INSTALL(ID3D11Device1Vtbl, GetImmediateContext1,  dev_get_context1);
    INSTALL(ID3D11Device2Vtbl, GetImmediateContext2,  dev_get_context2);
    INSTALL(ID3D11Device3Vtbl, GetImmediateContext3,  dev_get_context3);
    INSTALL(ID3D11DeviceVtbl,  CreateDeferredContext,  dev_deferred);
    INSTALL(ID3D11Device1Vtbl, CreateDeferredContext1, dev_deferred1);
    INSTALL(ID3D11Device2Vtbl, CreateDeferredContext2, dev_deferred2);
    INSTALL(ID3D11Device3Vtbl, CreateDeferredContext3, dev_deferred3);
    INSTALL(ID3D11DeviceVtbl,  CreateTexture1D, dev_create_tex1d);
    INSTALL(ID3D11DeviceVtbl,  CreateTexture2D, dev_create_tex2d);
    INSTALL(ID3D11DeviceVtbl,  CreateTexture3D, dev_create_tex3d);
    INSTALL(ID3D11Device3Vtbl, CreateTexture2D1, dev_create_tex2d1);
    INSTALL(ID3D11Device3Vtbl, CreateTexture3D1, dev_create_tex3d1);
    INSTALL(ID3D11DeviceVtbl,  CreateRenderTargetView, dev_create_rtv);
    INSTALL(ID3D11Device3Vtbl, CreateRenderTargetView1, dev_create_rtv1);
    INSTALL(ID3D11DeviceVtbl,  CreateShaderResourceView, dev_create_srv);
    INSTALL(ID3D11Device3Vtbl, CreateShaderResourceView1, dev_create_srv1);
    INSTALL(ID3D11DeviceVtbl,  CreateUnorderedAccessView, dev_create_uav);
    INSTALL(ID3D11Device3Vtbl, CreateUnorderedAccessView1, dev_create_uav1);
    INSTALL(ID3D11DeviceVtbl,  CreateDepthStencilView, dev_create_dsv);
    INSTALL(ID3D11DeviceVtbl,  CreateInputLayout, dev_create_layout);
    INSTALL(ID3D11DeviceVtbl,  CreateGeometryShaderWithStreamOutput, dev_create_gs_so);
    INSTALL(ID3D11DeviceVtbl,  CreateGeometryShader, dev_create_gs);
    INSTALL(ID3D11DeviceVtbl,  CreateVertexShader, dev_create_vs);
    INSTALL(ID3D11DeviceVtbl,  CreatePixelShader, dev_create_ps);
    INSTALL(ID3D11DeviceVtbl,  CheckMultisampleQualityLevels, dev_check_msaa);
    INSTALL(ID3D11Device2Vtbl, CheckMultisampleQualityLevels1, dev_check_msaa1);
    INSTALL(ID3D11DeviceVtbl,  CheckFeatureSupport, dev_check_feature);
    INSTALL(ID3D11DeviceVtbl,  CheckFormatSupport, dev_check_format);
#undef INSTALL
    return w;
}

/* --- entry points --- */

HRESULT WINAPI dm_d3d11_create_device(dxcts_d3d11_create_fn real, void *adapter,
                                      UINT driver_type, HMODULE software, UINT flags,
                                      const void *feature_levels,
                                      UINT num_feature_levels, UINT sdk_version,
                                      void **device, UINT *feature_level,
                                      void **context)
{
    dev_key key;
    int reusable;
    HRESULT hr;
    UINT level = 0;

    if (!dxcts_wrap_supported())
        return real(adapter, driver_type, software, flags, feature_levels,
                    num_feature_levels, sdk_version, device, feature_level, context);
    reusable = dev_key_of(adapter, driver_type, software, flags, feature_levels,
                          num_feature_levels, sdk_version, &key);
    if (reusable && device && dm_knob_device_reuse())
    {
        dm_device *parked = take_parked(&key);

        if (parked)
        {
            ID3D11DeviceContext *ctx = NULL;

            InterlockedIncrement(&parked->base.refs);
            ID3D11Device_GetImmediateContext((ID3D11Device *)parked->base.real, &ctx);
            if (ctx)
                ID3D11DeviceContext_ClearState(ctx);
            if (context)
                *context = ctx ? wrap_context(parked, ctx) : NULL;
            else
                dxcts_release(ctx);
            *device = parked;
            if (feature_level)
                *feature_level = parked->level;
            return S_OK;
        }
    }
    hr = real(adapter, driver_type, software, flags, feature_levels, num_feature_levels,
              sdk_version, device, &level, context);
    if (feature_level)
        *feature_level = level;
    if (SUCCEEDED(hr) && device && *device)
    {
        void *wrapped = wrap_device(*device, &key, reusable, level);

        if (context && *context)
            *context = wrapped == *device ? *context : wrap_context(wrapped, *context);
        *device = wrapped;
    }
    return hr;
}

/* The same device by another entry point; never parked, since the swapchain holds it. */
HRESULT WINAPI dm_d3d11_create_device_and_swapchain(
    dxcts_d3d11_create_swapchain_fn real, void *adapter, UINT driver_type,
    HMODULE software, UINT flags, const void *feature_levels, UINT num_feature_levels,
    UINT sdk_version, const void *swapchain_desc, void **swapchain, void **device,
    UINT *feature_level, void **context)
{
    HRESULT hr;
    UINT level = 0;

    if (!dxcts_wrap_supported())
        return real(adapter, driver_type, software, flags, feature_levels,
                    num_feature_levels, sdk_version, swapchain_desc, swapchain, device,
                    feature_level, context);
    hr = real(adapter, driver_type, software, flags, feature_levels, num_feature_levels,
              sdk_version, swapchain_desc, swapchain, device, &level, context);
    if (feature_level)
        *feature_level = level;
    if (SUCCEEDED(hr) && device && *device)
    {
        void *wrapped = wrap_device(*device, NULL, 0, level);

        if (context && *context)
            *context = wrapped == *device ? *context : wrap_context(wrapped, *context);
        *device = wrapped;
    }
    return hr;
}
