/*
 * The D3DMetal layer's D3D12 half: a wrapped device and wrapped graphics command
 * lists carrying the D3D12 workarounds listed in backend.c.
 */

#include "d3dmetal.h"

typedef HRESULT (WINAPI *check_feature_fn)(void *, UINT, void *, UINT);
typedef HRESULT (WINAPI *create_committed_fn)(void *, const D3D12_HEAP_PROPERTIES *,
                                             D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                             D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                             const GUID *, void **);
typedef HRESULT (WINAPI *create_committed1_fn)(void *, const D3D12_HEAP_PROPERTIES *,
                                              D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                              D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE *, void *,
                                              const GUID *, void **);
typedef HRESULT (WINAPI *create_committed2_fn)(void *, const D3D12_HEAP_PROPERTIES *,
                                              D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *,
                                              D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE *, void *,
                                              const GUID *, void **);
typedef HRESULT (WINAPI *create_placed_fn)(void *, void *, UINT64,
                                          const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                          const D3D12_CLEAR_VALUE *, const GUID *, void **);
typedef HRESULT (WINAPI *create_placed1_fn)(void *, void *, UINT64,
                                           const D3D12_RESOURCE_DESC1 *,
                                           D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                           const GUID *, void **);
typedef HRESULT (WINAPI *create_reserved_fn)(void *, const D3D12_RESOURCE_DESC *,
                                            D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                            const GUID *, void **);
typedef HRESULT (WINAPI *create_reserved1_fn)(void *, const D3D12_RESOURCE_DESC *,
                                             D3D12_RESOURCE_STATES,
                                             const D3D12_CLEAR_VALUE *, void *,
                                             const GUID *, void **);
typedef void (WINAPI *create_rtv_fn)(void *, ID3D12Resource *,
                                     const D3D12_RENDER_TARGET_VIEW_DESC *,
                                     D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT (WINAPI *create_gfx_pso_fn)(void *,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                           const GUID *, void **);
typedef HRESULT (WINAPI *create_cmdlist_fn)(void *, UINT, D3D12_COMMAND_LIST_TYPE, void *,
                                           void *, const GUID *, void **);
typedef HRESULT (WINAPI *create_cmdlist1_fn)(void *, UINT, D3D12_COMMAND_LIST_TYPE,
                                            D3D12_COMMAND_LIST_FLAGS, const GUID *, void **);
typedef void (WINAPI *barrier_fn)(void *, UINT, const D3D12_RESOURCE_BARRIER *);
typedef void (WINAPI *set_ib_fn)(void *, const D3D12_INDEX_BUFFER_VIEW *);
typedef void (WINAPI *draw_indexed_fn)(void *, UINT, UINT, UINT, INT, UINT);
typedef HRESULT (WINAPI *reset_fn)(void *, void *, void *);
typedef void (WINAPI *resolve_region_fn)(void *, ID3D12Resource *, UINT, UINT, UINT,
                                         ID3D12Resource *, UINT, D3D12_RECT *, DXGI_FORMAT,
                                         D3D12_RESOLVE_MODE);

#define REAL_DEV(self, method) dxcts_real_vtable(self)[SLOT(ID3D12DeviceVtbl, method)]
#define REAL_CL(self, method) \
    dxcts_real_vtable(self)[SLOT(ID3D12GraphicsCommandListVtbl, method)]

/* COM methods return a struct through a hidden pointer passed after `this`, on every
 * architecture - the RetVal parameter d3d12.h's C interface spells out.  On arm64 this
 * is not the C convention for a function returning the struct by value (x8, or x0:x1
 * for 16 bytes), so the call must be declared this way rather than by value. */
typedef D3D12_RESOURCE_DESC *(WINAPI *res_get_desc_fn)(void *, D3D12_RESOURCE_DESC *);

static void resource_desc(ID3D12Resource *r, D3D12_RESOURCE_DESC *out)
{
    ((res_get_desc_fn)(*(void ***)r)[SLOT(ID3D12ResourceVtbl, GetDesc)])(r, out);
}

static const dxcts_iface k_devices[] = {
    { &IID_ID3D12Device, WIDTH(ID3D12DeviceVtbl) },
    { &IID_ID3D12Device1, WIDTH(ID3D12Device1Vtbl) },
    { &IID_ID3D12Device2, WIDTH(ID3D12Device2Vtbl) },
    { &IID_ID3D12Device3, WIDTH(ID3D12Device3Vtbl) },
    { &IID_ID3D12Device4, WIDTH(ID3D12Device4Vtbl) },
    { &IID_ID3D12Device5, WIDTH(ID3D12Device5Vtbl) },
    { &IID_ID3D12Device6, WIDTH(ID3D12Device6Vtbl) },
    { &IID_ID3D12Device7, WIDTH(ID3D12Device7Vtbl) },
    { &IID_ID3D12Device8, WIDTH(ID3D12Device8Vtbl) },
    { &IID_ID3D12Device9, WIDTH(ID3D12Device9Vtbl) },
    { &IID_ID3D12Device10, WIDTH(ID3D12Device10Vtbl) },
};

/* ID3D12CommandList is the base every graphics list also answers to; its width is
 * below the graphics list's, so the wrapper covers it too. */
static const dxcts_iface k_cmdlists[] = {
    { &IID_ID3D12CommandList, WIDTH(ID3D12CommandListVtbl) },
    { &IID_ID3D12GraphicsCommandList, WIDTH(ID3D12GraphicsCommandListVtbl) },
    { &IID_ID3D12GraphicsCommandList1, WIDTH(ID3D12GraphicsCommandList1Vtbl) },
    { &IID_ID3D12GraphicsCommandList2, WIDTH(ID3D12GraphicsCommandList2Vtbl) },
    { &IID_ID3D12GraphicsCommandList3, WIDTH(ID3D12GraphicsCommandList3Vtbl) },
    { &IID_ID3D12GraphicsCommandList4, WIDTH(ID3D12GraphicsCommandList4Vtbl) },
    { &IID_ID3D12GraphicsCommandList5, WIDTH(ID3D12GraphicsCommandList5Vtbl) },
    { &IID_ID3D12GraphicsCommandList6, WIDTH(ID3D12GraphicsCommandList6Vtbl) },
    { &IID_ID3D12GraphicsCommandList7, WIDTH(ID3D12GraphicsCommandList7Vtbl) },
};

/* --- command lists --- */

typedef struct
{
    dxcts_wrapper base;
    int has_ib;
} dm_cmdlist;

static HRESULT WINAPI cl_qi(void *self, const GUID *iid, void **out)
{
    dm_cmdlist *w = self;

    if (iid && out && dxcts_wrapper_answers(&w->base, iid, k_cmdlists,
                                            DXCTS_COUNT(k_cmdlists)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return ((dxcts_qi_fn)dxcts_real_vtable(self)[0])(dxcts_real(self), iid, out);
}

/* A transition of a buffer to or from the render-target state (the companion of a
 * buffer render-target view, which CreateRenderTargetView below skips) reaches
 * D3DMetal's transition barrier, which reads texture state the MTLBuffer does not
 * have.  Buffers need no layout transition in Metal, so those entries are dropped and
 * the rest of the batch recorded. */
static void WINAPI cl_resource_barrier(void *self, UINT n, const D3D12_RESOURCE_BARRIER *barriers)
{
    barrier_fn real = (barrier_fn)REAL_CL(self, ResourceBarrier);
    D3D12_RESOURCE_BARRIER kept[64];
    UINT i, k = 0;

    if (!dm_knob_create_guards() || !n || !barriers || n > 64)
    {
        real(dxcts_real(self), n, barriers);
        return;
    }
    for (i = 0; i < n; i++)
    {
        const D3D12_RESOURCE_BARRIER *b = &barriers[i];

        if (b->Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b->Transition.pResource
            && ((b->Transition.StateBefore | b->Transition.StateAfter)
                & D3D12_RESOURCE_STATE_RENDER_TARGET))
        {
            D3D12_RESOURCE_DESC rd;

            resource_desc(b->Transition.pResource, &rd);
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                continue;
        }
        kept[k++] = *b;
    }
    if (k)
        real(dxcts_real(self), k, kept);
}

/* An indexed draw recorded with no index buffer bound hands Metal a nil index buffer
 * and the GPU driver faults inside drawIndexedPrimitives; such a draw is skipped. */
static void WINAPI cl_set_index_buffer(void *self, const D3D12_INDEX_BUFFER_VIEW *view)
{
    ((set_ib_fn)REAL_CL(self, IASetIndexBuffer))(dxcts_real(self), view);
    ((dm_cmdlist *)self)->has_ib = view && view->BufferLocation;
}

static HRESULT WINAPI cl_reset(void *self, void *allocator, void *pso)
{
    ((dm_cmdlist *)self)->has_ib = 0;
    return ((reset_fn)REAL_CL(self, Reset))(dxcts_real(self), allocator, pso);
}

static void WINAPI cl_draw_indexed_instanced(void *self, UINT index_count, UINT instance_count,
                                             UINT start_index, INT base_vertex,
                                             UINT start_instance)
{
    if (dm_knob_create_guards() && !((dm_cmdlist *)self)->has_ib)
    {
        dm_count(&dm_counts.dropped_draws);
        return;
    }
    ((draw_indexed_fn)REAL_CL(self, DrawIndexedInstanced))(dxcts_real(self), index_count,
                                                           instance_count, start_index,
                                                           base_vertex, start_instance);
}

/* A MIN or MAX resolve of a depth resource: D3DMetal builds it as a draw into a colour
 * target of the depth format (Depth24Unorm_Stencil8, Depth32Float, Depth16Unorm), Metal
 * refuses that format as a colour render target, and its validation aborts the process
 * when the pipeline is built.  Such a resolve is skipped - unless Metal is in this
 * process and told to report instead of aborting (MTL_ERROR_MODE, which Wine carries
 * over from the Unix environment; see the README): D3DMetal then gets past the refused
 * pipeline and the resolve is right (ProgrammableMSAA's Tier1 passes under Wine), so
 * skipping it would only cost the case. */
static int metal_aborts(void)
{
    static LONG cached;    /* 0 unknown, 1 aborts, 2 reports */
    char mode[16];

    if (!cached)
        InterlockedExchange(&cached,
                            GetEnvironmentVariableA("MTL_ERROR_MODE", mode, sizeof(mode)) ? 2 : 1);
    return cached == 1;
}

static void WINAPI cl_resolve_region(void *self, ID3D12Resource *dst, UINT dst_sub, UINT x,
                                     UINT y, ID3D12Resource *src, UINT src_sub,
                                     D3D12_RECT *rect, DXGI_FORMAT format,
                                     D3D12_RESOLVE_MODE mode)
{
    if (dm_knob_draw_guards() && src && metal_aborts()
        && (mode == D3D12_RESOLVE_MODE_MIN || mode == D3D12_RESOLVE_MODE_MAX))
    {
        D3D12_RESOURCE_DESC rd;

        resource_desc(src, &rd);
        if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        {
            dm_count(&dm_counts.dropped_draws);
            return;
        }
    }
    ((resolve_region_fn)dxcts_real_vtable(self)[SLOT(ID3D12GraphicsCommandList1Vtbl,
                                                     ResolveSubresourceRegion)])(
        dxcts_real(self), dst, dst_sub, x, y, src, src_sub, rect, format, mode);
}

static void *wrap_cmdlist(void *list)
{
    static int probed_slots;
    dm_cmdlist *w;
    int slots = dxcts_widest_slots(list, k_cmdlists,
                                   DXCTS_COUNT(k_cmdlists), &probed_slots);

    /* A list that is not a graphics list (a bundle or a compute/copy list still
     * answers ID3D12GraphicsCommandList; only a video list would not) is passed
     * through: nothing here applies to it. */
    if (slots < WIDTH(ID3D12GraphicsCommandListVtbl))
        return list;
    w = (dm_cmdlist *)dxcts_wrap(list, slots, sizeof(*w));
    if (!w)
        return list;
    w->base.vtable[0] = (void *)cl_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)dxcts_wrapper_release;
    DXCTS_INSTALL(&w->base, ID3D12GraphicsCommandListVtbl, ResourceBarrier,
                  cl_resource_barrier);
    DXCTS_INSTALL(&w->base, ID3D12GraphicsCommandListVtbl, IASetIndexBuffer,
                  cl_set_index_buffer);
    DXCTS_INSTALL(&w->base, ID3D12GraphicsCommandListVtbl, Reset, cl_reset);
    DXCTS_INSTALL(&w->base, ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced,
                  cl_draw_indexed_instanced);
    DXCTS_INSTALL(&w->base, ID3D12GraphicsCommandList1Vtbl, ResolveSubresourceRegion,
                  cl_resolve_region);
    return w;
}

static HRESULT WINAPI dev_create_cmdlist(void *self, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                         void *allocator, void *pso, const GUID *iid,
                                         void **out)
{
    HRESULT hr = ((create_cmdlist_fn)REAL_DEV(self, CreateCommandList))(
        dxcts_real(self), node, type, allocator, pso, iid, out);

    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE)
        *out = wrap_cmdlist(*out);
    return hr;
}

static HRESULT WINAPI dev_create_cmdlist1(void *self, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                          D3D12_COMMAND_LIST_FLAGS flags, const GUID *iid,
                                          void **out)
{
    HRESULT hr = ((create_cmdlist1_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device4Vtbl,
                                                                    CreateCommandList1)])(
        dxcts_real(self), node, type, flags, iid, out);

    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE)
        *out = wrap_cmdlist(*out);
    return hr;
}

/* --- command queues --- */

/* Microsoft's runtime reads its own fields out of every list ExecuteCommandLists is
 * given, so a list wrapped above must reach it as the real object.  The queue is not
 * wrapped where that can be helped - DXGI and D3D11On12 take queues and look inside
 * them too - so its vtable slot is patched instead, once per vtable.  Where the vtable
 * cannot be written (the host implementation under Wine, see comwrap.h) the queue is
 * wrapped after all (queue_out below). */
typedef void (WINAPI *execute_fn)(void *, UINT, ID3D12CommandList *const *);
typedef HRESULT (WINAPI *create_queue_fn)(void *, const D3D12_COMMAND_QUEUE_DESC *,
                                         const GUID *, void **);
typedef HRESULT (WINAPI *create_queue1_fn)(void *, const D3D12_COMMAND_QUEUE_DESC *,
                                          const GUID *, const GUID *, void **);

static struct
{
    void **vtable;
    execute_fn real;
} queue_vtables[8];
static int queue_vtable_count;
static SRWLOCK queue_lock = SRWLOCK_INIT;

static execute_fn queue_real_execute(void **vtable)
{
    execute_fn real = NULL;
    int i;

    AcquireSRWLockShared(&queue_lock);
    for (i = 0; i < queue_vtable_count && !real; i++)
        if (queue_vtables[i].vtable == vtable)
            real = queue_vtables[i].real;
    ReleaseSRWLockShared(&queue_lock);
    return real;
}

static void *unwrap_cmdlist(ID3D12CommandList *list)
{
    if (list && (*(void ***)list)[0] == (void *)cl_qi)
        return dxcts_real(list);
    return list;
}

/* Hands `lists` to `real` with every wrapped list replaced by the list it wraps. */
static void execute_unwrapped(execute_fn real, void *queue, UINT n,
                              ID3D12CommandList *const *lists)
{
    ID3D12CommandList *local[64], **unwrapped = local;
    UINT i;

    if (n > 64 && !(unwrapped = malloc(n * sizeof(*unwrapped))))
        return;
    for (i = 0; i < n; i++)
        unwrapped[i] = unwrap_cmdlist(lists[i]);
    real(queue, n, unwrapped);
    if (unwrapped != local)
        free(unwrapped);
}

static void WINAPI queue_execute(void *self, UINT n, ID3D12CommandList *const *lists)
{
    execute_fn real = queue_real_execute(*(void ***)self);

    if (real)
        execute_unwrapped(real, self, n, lists);
}

/* Whether the queue's ExecuteCommandLists now unwraps (patched now or before). */
static int patch_queue(void *object)
{
    void *queue = NULL, **vtable, **slot;
    HMODULE self;
    DWORD old;
    int patched;

    if (FAILED(((dxcts_qi_fn)(*(void ***)object)[0])(object, &IID_ID3D12CommandQueue,
                                                     &queue)))
        return 0;
    vtable = *(void ***)queue;
    slot = &vtable[SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists)];
    AcquireSRWLockExclusive(&queue_lock);
    patched = *slot == (void *)queue_execute;
    if (!patched
        && queue_vtable_count < (int)DXCTS_COUNT(queue_vtables)
        && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
    {
        /* The patched slot points into this DLL for the rest of the process. */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)queue_execute, &self);
        queue_vtables[queue_vtable_count].vtable = vtable;
        queue_vtables[queue_vtable_count].real = (execute_fn)*slot;
        queue_vtable_count++;
        *slot = (void *)queue_execute;
        VirtualProtect(slot, sizeof(*slot), old, &old);
        patched = 1;
    }
    ReleaseSRWLockExclusive(&queue_lock);
    dxcts_release(queue);
    return patched;
}

/* Where the slot cannot be patched the queue itself is wrapped, so that lists reach
 * the host unwrapped all the same: under Wine, D3DMetal's ExecuteCommandLists reads a
 * wrapped list as its own object and faults on the first one (every D3D12 group with
 * the layer died that way).  Whatever else takes the queue - DXGI, D3D11On12 - asks
 * it for an interface first, and every query but the queue's own reaches the real
 * object. */
static const dxcts_iface k_queues[] = {
    { &IID_ID3D12CommandQueue, WIDTH(ID3D12CommandQueueVtbl) },
};

static HRESULT WINAPI queue_qi(void *self, const GUID *iid, void **out)
{
    dxcts_wrapper *w = self;

    if (iid && out && dxcts_wrapper_answers(w, iid, k_queues,
                                            DXCTS_COUNT(k_queues)))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return ((dxcts_qi_fn)dxcts_real_vtable(self)[0])(dxcts_real(self), iid, out);
}

static void WINAPI queue_wrapper_execute(void *self, UINT n, ID3D12CommandList *const *lists)
{
    execute_unwrapped((execute_fn)dxcts_real_vtable(self)[SLOT(ID3D12CommandQueueVtbl,
                                                                ExecuteCommandLists)],
                      dxcts_real(self), n, lists);
}

static void *queue_out(void *object, const GUID *iid)
{
    static int probed_slots;
    dxcts_wrapper *w;

    if (patch_queue(object) || !iid || !IsEqualGUID(iid, &IID_ID3D12CommandQueue)
        || !dxcts_wrap_supported())
        return object;
    w = dxcts_wrap(object, dxcts_widest_slots(object, k_queues,
                                              DXCTS_COUNT(k_queues),
                                              &probed_slots),
                   sizeof(*w));
    if (!w)
        return object;
    w->vtable[0] = (void *)queue_qi;
    w->vtable[1] = (void *)dxcts_wrapper_addref;
    w->vtable[2] = (void *)dxcts_wrapper_release;
    DXCTS_INSTALL(w, ID3D12CommandQueueVtbl, ExecuteCommandLists, queue_wrapper_execute);
    return w;
}

static HRESULT WINAPI dev_create_queue(void *self, const D3D12_COMMAND_QUEUE_DESC *desc,
                                       const GUID *iid, void **out)
{
    HRESULT hr = ((create_queue_fn)REAL_DEV(self, CreateCommandQueue))(dxcts_real(self), desc,
                                                                       iid, out);

    if (SUCCEEDED(hr) && out && *out)
        *out = queue_out(*out, iid);
    return hr;
}

static HRESULT WINAPI dev_create_queue1(void *self, const D3D12_COMMAND_QUEUE_DESC *desc,
                                        const GUID *creator, const GUID *iid, void **out)
{
    HRESULT hr = ((create_queue1_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device9Vtbl,
                                                                  CreateCommandQueue1)])(
        dxcts_real(self), desc, creator, iid, out);

    if (SUCCEEDED(hr) && out && *out)
        *out = queue_out(*out, iid);
    return hr;
}

/* --- resources --- */

/* A texture-shaped description Metal cannot make (buffers are never refused). */
static int unmakeable(const D3D12_RESOURCE_DESC *desc)
{
    return desc && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
        && dm_knob_texture_guard()
        && !dm_format_texture_ok(desc->Format,
                                 desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                 desc->SampleDesc.Count);
}

static int refuse_desc(const D3D12_RESOURCE_DESC *desc, void **out)
{
    if (!unmakeable(desc))
        return 0;
    if (out)
        *out = NULL;
    return 1;
}

/* D3D12_RESOURCE_DESC1 is D3D12_RESOURCE_DESC followed by the sampler feedback mip
 * region, so the shared prefix is examined. */
_Static_assert(offsetof(D3D12_RESOURCE_DESC1, Format) == offsetof(D3D12_RESOURCE_DESC, Format)
               && offsetof(D3D12_RESOURCE_DESC1, SampleDesc)
                  == offsetof(D3D12_RESOURCE_DESC, SampleDesc)
               && offsetof(D3D12_RESOURCE_DESC1, Flags) == offsetof(D3D12_RESOURCE_DESC, Flags)
               && offsetof(D3D12_RESOURCE_DESC1, Dimension)
                  == offsetof(D3D12_RESOURCE_DESC, Dimension),
               "D3D12_RESOURCE_DESC1 no longer starts with D3D12_RESOURCE_DESC");

static HRESULT WINAPI dev_create_committed(void *self, const D3D12_HEAP_PROPERTIES *hp,
                                           D3D12_HEAP_FLAGS flags,
                                           const D3D12_RESOURCE_DESC *desc,
                                           D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE *clear, const GUID *iid,
                                           void **out)
{
    if (refuse_desc(desc, out))
        return E_INVALIDARG;
    return ((create_committed_fn)REAL_DEV(self, CreateCommittedResource))(
        dxcts_real(self), hp, flags, desc, state, clear, iid, out);
}

static HRESULT WINAPI dev_create_committed1(void *self, const D3D12_HEAP_PROPERTIES *hp,
                                            D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC *desc,
                                            D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE *clear, void *session,
                                            const GUID *iid, void **out)
{
    if (refuse_desc(desc, out))
        return E_INVALIDARG;
    return ((create_committed1_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device4Vtbl,
                                                                CreateCommittedResource1)])(
        dxcts_real(self), hp, flags, desc, state, clear, session, iid, out);
}

static HRESULT WINAPI dev_create_committed2(void *self, const D3D12_HEAP_PROPERTIES *hp,
                                            D3D12_HEAP_FLAGS flags,
                                            const D3D12_RESOURCE_DESC1 *desc,
                                            D3D12_RESOURCE_STATES state,
                                            const D3D12_CLEAR_VALUE *clear, void *session,
                                            const GUID *iid, void **out)
{
    if (refuse_desc((const D3D12_RESOURCE_DESC *)desc, out))
        return E_INVALIDARG;
    return ((create_committed2_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device8Vtbl,
                                                                CreateCommittedResource2)])(
        dxcts_real(self), hp, flags, desc, state, clear, session, iid, out);
}

static HRESULT WINAPI dev_create_placed(void *self, void *heap, UINT64 offset,
                                        const D3D12_RESOURCE_DESC *desc,
                                        D3D12_RESOURCE_STATES state,
                                        const D3D12_CLEAR_VALUE *clear, const GUID *iid,
                                        void **out)
{
    if (refuse_desc(desc, out))
        return E_INVALIDARG;
    return ((create_placed_fn)REAL_DEV(self, CreatePlacedResource))(
        dxcts_real(self), heap, offset, desc, state, clear, iid, out);
}

static HRESULT WINAPI dev_create_placed1(void *self, void *heap, UINT64 offset,
                                         const D3D12_RESOURCE_DESC1 *desc,
                                         D3D12_RESOURCE_STATES state,
                                         const D3D12_CLEAR_VALUE *clear, const GUID *iid,
                                         void **out)
{
    if (refuse_desc((const D3D12_RESOURCE_DESC *)desc, out))
        return E_INVALIDARG;
    return ((create_placed1_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device8Vtbl,
                                                             CreatePlacedResource1)])(
        dxcts_real(self), heap, offset, desc, state, clear, iid, out);
}

static HRESULT WINAPI dev_create_reserved(void *self, const D3D12_RESOURCE_DESC *desc,
                                          D3D12_RESOURCE_STATES state,
                                          const D3D12_CLEAR_VALUE *clear, const GUID *iid,
                                          void **out)
{
    if (refuse_desc(desc, out))
        return E_INVALIDARG;
    return ((create_reserved_fn)REAL_DEV(self, CreateReservedResource))(
        dxcts_real(self), desc, state, clear, iid, out);
}

static HRESULT WINAPI dev_create_reserved1(void *self, const D3D12_RESOURCE_DESC *desc,
                                           D3D12_RESOURCE_STATES state,
                                           const D3D12_CLEAR_VALUE *clear, void *session,
                                           const GUID *iid, void **out)
{
    if (refuse_desc(desc, out))
        return E_INVALIDARG;
    return ((create_reserved1_fn)dxcts_real_vtable(self)[SLOT(ID3D12Device4Vtbl,
                                                               CreateReservedResource1)])(
        dxcts_real(self), desc, state, clear, session, iid, out);
}

/*
 * The allocation-info query builds the same MTLTextureDescriptor a create does, and
 * Metal's validation abort()s on the same descriptions, so the texture refusal applies
 * here too.  D3D12 reports an invalid description as SizeInBytes == UINT64_MAX.  The
 * 16-byte result comes back through the hidden pointer, as for GetDesc above.
 */
static int alloc_info_refused(UINT n, const D3D12_RESOURCE_DESC *descs)
{
    UINT i;

    for (i = 0; descs && i < n; i++)
        if (unmakeable(&descs[i]))
        {
            return 1;
        }
    return 0;
}

typedef D3D12_RESOURCE_ALLOCATION_INFO *(WINAPI *alloc_info_fn)(
    void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT, const D3D12_RESOURCE_DESC *);

static D3D12_RESOURCE_ALLOCATION_INFO *WINAPI dev_alloc_info(
    void *self, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT n,
    const D3D12_RESOURCE_DESC *descs)
{
    if (alloc_info_refused(n, descs))
    {
        if (ret)
        {
            ret->SizeInBytes = UINT64_MAX;
            ret->Alignment = 0;
        }
        return ret;
    }
    return ((alloc_info_fn)REAL_DEV(self, GetResourceAllocationInfo))(dxcts_real(self), ret,
                                                                       mask, n, descs);
}

/* A render-target view of a buffer (D3D12_RTV_DIMENSION_BUFFER, legal in D3D12)
 * reaches D3D12Texture::GetView, which sends texture selectors to the MTLBuffer and
 * aborts the process.  The create is void, so the descriptor is left unwritten: a draw
 * through it is undefined rather than fatal. */
static void WINAPI dev_create_rtv(void *self, ID3D12Resource *res,
                                  const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (dm_knob_create_guards())
    {
        int buffer = desc && desc->ViewDimension == D3D12_RTV_DIMENSION_BUFFER;

        if (!buffer && res)
        {
            D3D12_RESOURCE_DESC rd;

            resource_desc(res, &rd);
            buffer = rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        }
        if (buffer)
        {
            return;
        }
    }
    ((create_rtv_fn)REAL_DEV(self, CreateRenderTargetView))(dxcts_real(self), res, desc, handle);
}

/* A stream-output entry with no semantic name (a declaration gap) reaches D3DMetal's
 * pipeline cache key as a NULL string it strlen()s; refused, as the D3D11 create with
 * the same gap is, and so is a declaration over the 53 entries its table holds. */
static HRESULT WINAPI dev_create_gfx_pso(void *self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                         const GUID *iid, void **out)
{
    if (dm_knob_create_guards() && desc && desc->StreamOutput.NumEntries)
    {
        int bad = !desc->StreamOutput.pSODeclaration
               || desc->StreamOutput.NumEntries > DM_MAX_STREAM_OUTPUT_ENTRIES;
        UINT i;

        for (i = 0; !bad && i < desc->StreamOutput.NumEntries; i++)
            bad = !desc->StreamOutput.pSODeclaration[i].SemanticName;
        if (bad)
        {
            return dm_refuse(out);
        }
    }
    /* A geometry shader under tessellation: D3DMetal cannot compile the geometry
     * function, hands Metal a mesh pipeline with no mesh function, and validation aborts
     * the process when the first draw builds it.  D3D11 drops such a draw; in D3D12 the
     * combination is fixed at create, so the pipeline state is refused there. */
    if (dm_knob_draw_guards() && desc && desc->GS.BytecodeLength
        && (desc->HS.BytecodeLength || desc->DS.BytecodeLength))
    {
        return dm_refuse(out);
    }
    return ((create_gfx_pso_fn)REAL_DEV(self, CreateGraphicsPipelineState))(dxcts_real(self),
                                                                            desc, iid, out);
}

/* --- CheckFeatureSupport --- */

#define FEATURE_SIZE(id, type) case id: return sizeof(type)

static UINT feature_data_size(UINT feature)
{
    switch (feature)
    {
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS, D3D12_FEATURE_DATA_D3D12_OPTIONS);
    FEATURE_SIZE(D3D12_FEATURE_ARCHITECTURE, D3D12_FEATURE_DATA_ARCHITECTURE);
    FEATURE_SIZE(D3D12_FEATURE_FORMAT_SUPPORT, D3D12_FEATURE_DATA_FORMAT_SUPPORT);
    FEATURE_SIZE(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                 D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS);
    FEATURE_SIZE(D3D12_FEATURE_FORMAT_INFO, D3D12_FEATURE_DATA_FORMAT_INFO);
    FEATURE_SIZE(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,
                 D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
    FEATURE_SIZE(D3D12_FEATURE_SHADER_MODEL, D3D12_FEATURE_DATA_SHADER_MODEL);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS1, D3D12_FEATURE_DATA_D3D12_OPTIONS1);
    FEATURE_SIZE(D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT,
                 D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT);
    FEATURE_SIZE(D3D12_FEATURE_ROOT_SIGNATURE, D3D12_FEATURE_DATA_ROOT_SIGNATURE);
    FEATURE_SIZE(D3D12_FEATURE_ARCHITECTURE1, D3D12_FEATURE_DATA_ARCHITECTURE1);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS2, D3D12_FEATURE_DATA_D3D12_OPTIONS2);
    FEATURE_SIZE(D3D12_FEATURE_SHADER_CACHE, D3D12_FEATURE_DATA_SHADER_CACHE);
    FEATURE_SIZE(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,
                 D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS3, D3D12_FEATURE_DATA_D3D12_OPTIONS3);
    FEATURE_SIZE(D3D12_FEATURE_EXISTING_HEAPS, D3D12_FEATURE_DATA_EXISTING_HEAPS);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS4, D3D12_FEATURE_DATA_D3D12_OPTIONS4);
    FEATURE_SIZE(D3D12_FEATURE_SERIALIZATION, D3D12_FEATURE_DATA_SERIALIZATION);
    FEATURE_SIZE(D3D12_FEATURE_CROSS_NODE, D3D12_FEATURE_DATA_CROSS_NODE);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS5, D3D12_FEATURE_DATA_D3D12_OPTIONS5);
    FEATURE_SIZE(D3D12_FEATURE_DISPLAYABLE, D3D12_FEATURE_DATA_DISPLAYABLE);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS6, D3D12_FEATURE_DATA_D3D12_OPTIONS6);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS7, D3D12_FEATURE_DATA_D3D12_OPTIONS7);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS8, D3D12_FEATURE_DATA_D3D12_OPTIONS8);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS9, D3D12_FEATURE_DATA_D3D12_OPTIONS9);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS10, D3D12_FEATURE_DATA_D3D12_OPTIONS10);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS11, D3D12_FEATURE_DATA_D3D12_OPTIONS11);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS12, D3D12_FEATURE_DATA_D3D12_OPTIONS12);
    FEATURE_SIZE(D3D12_FEATURE_D3D12_OPTIONS13, D3D12_FEATURE_DATA_D3D12_OPTIONS13);
    default: return 0;
    }
}

#undef FEATURE_SIZE

/* "Not supported" is an all-zero struct for most features, but three of the
 * enumerations D3D12 answers with have no zero, so zeroing would invent a value the
 * enumeration does not have.  Those get their lowest legal value instead. */
static void feature_report_unsupported(UINT feature, void *data)
{
    switch (feature)
    {
    case D3D12_FEATURE_D3D12_OPTIONS:
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS *o = data;

        o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
        break;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE:
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE *o = data;

        o->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        break;
    }
    case D3D12_FEATURE_SHADER_MODEL:
    {
        D3D12_FEATURE_DATA_SHADER_MODEL *o = data;

        o->HighestShaderModel = D3D_SHADER_MODEL_5_1;
        break;
    }
    default:
        break;
    }
}

/*
 * Measured: D3D12_FEATURE_ROOT_SIGNATURE, D3D12_FEATURE_SERIALIZATION and
 * D3D12_FEATURE_D3D12_OPTIONS5 return E_INVALIDARG and the host logs "unhandled support
 * query" for them; a conformance binary that asks about one dies with no case run.
 * Reporting the feature unsupported is what a device that lacks it says.  The
 * multisample quality levels get the same correction as the D3D11 query.
 */
static HRESULT WINAPI dev_check_feature(void *self, UINT feature, void *data, UINT size)
{
    check_feature_fn fn = (check_feature_fn)REAL_DEV(self, CheckFeatureSupport);
    HRESULT hr = fn(dxcts_real(self), feature, data, size);
    UINT expected = feature_data_size(feature);

    if (SUCCEEDED(hr) && dm_knob_msaa_levels() && feature == D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS
        && data && size == expected)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *q = data;

        if (!dm_format_msaa_ok(q->Format, q->SampleCount))
            q->NumQualityLevels = 0;
    }
    if (hr == E_INVALIDARG && dm_knob_d3d12_feature_support() && data && expected
        && size == expected)
    {
        memset(data, 0, size);
        feature_report_unsupported(feature, data);
        return S_OK;
    }
    return hr;
}

/* --- device wrapper --- */

static HRESULT WINAPI dev_qi(void *self, const GUID *iid, void **out)
{
    dxcts_wrapper *w = self;
    dxcts_qi_fn qi = (dxcts_qi_fn)dxcts_real_vtable(self)[0];
    HRESULT hr;

    if (!out)
        return E_POINTER;
    hr = qi(w->real, iid, out);
    /* Every ID3D12Device revision is one object behind one vtable, so an interface that
     * comes back as the device itself is one this wrapper covers. */
    if (SUCCEEDED(hr) && *out == w->real)
    {
        dxcts_release(*out);
        dxcts_wrapper_addref(w);
        *out = w;
    }
    return hr;
}

HRESULT WINAPI dm_d3d12_create_device(dxcts_d3d12_create_fn real, void *adapter,
                                      UINT min_feature_level, const GUID *iid,
                                      void **device)
{
    static int probed_slots;
    HRESULT hr = real(adapter, min_feature_level, iid, device);
    dxcts_wrapper *w;

    if (FAILED(hr) || !device || !*device || !dxcts_wrap_supported())
        return hr;
    w = dxcts_wrap(*device, dxcts_widest_slots(*device, k_devices,
                                               DXCTS_COUNT(k_devices),
                                               &probed_slots),
                   sizeof(*w));
    if (!w)
        return hr;
    w->vtable[0] = (void *)dev_qi;
    w->vtable[1] = (void *)dxcts_wrapper_addref;
    w->vtable[2] = (void *)dxcts_wrapper_release;
#define INSTALL(vtbl, method, fn) DXCTS_INSTALL(w, vtbl, method, fn)
    INSTALL(ID3D12DeviceVtbl, CheckFeatureSupport, dev_check_feature);
    INSTALL(ID3D12DeviceVtbl, CreateCommittedResource, dev_create_committed);
    INSTALL(ID3D12Device4Vtbl, CreateCommittedResource1, dev_create_committed1);
    INSTALL(ID3D12Device8Vtbl, CreateCommittedResource2, dev_create_committed2);
    INSTALL(ID3D12DeviceVtbl, CreatePlacedResource, dev_create_placed);
    INSTALL(ID3D12Device8Vtbl, CreatePlacedResource1, dev_create_placed1);
    INSTALL(ID3D12DeviceVtbl, CreateReservedResource, dev_create_reserved);
    INSTALL(ID3D12Device4Vtbl, CreateReservedResource1, dev_create_reserved1);
    INSTALL(ID3D12DeviceVtbl, GetResourceAllocationInfo, dev_alloc_info);
    INSTALL(ID3D12DeviceVtbl, CreateRenderTargetView, dev_create_rtv);
    INSTALL(ID3D12DeviceVtbl, CreateGraphicsPipelineState, dev_create_gfx_pso);
    INSTALL(ID3D12DeviceVtbl, CreateCommandList, dev_create_cmdlist);
    INSTALL(ID3D12Device4Vtbl, CreateCommandList1, dev_create_cmdlist1);
    INSTALL(ID3D12DeviceVtbl, CreateCommandQueue, dev_create_queue);
    INSTALL(ID3D12Device9Vtbl, CreateCommandQueue1, dev_create_queue1);
#undef INSTALL
    *device = w;
    return hr;
}
