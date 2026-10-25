/*
 * What the layer checks at draw time, and the context state it tracks to do it.
 */

#include "d3d11_layer.h"

/* --- draw-time guards --- */
/*
 * Pipeline shapes D3DMetal accepts at bind time and dies on when it encodes the draw:
 *  - a tessellation patch whose vertex-shader output does not fit the 0x3b80 bytes
 *    of threadgroup memory D3DMetal budgets per patch (control points x bytes per
 *    output vertex): it computes zero patches per threadgroup and divides by that;
 *  - alpha-to-coverage on a render target Metal cannot blend: pipeline validation
 *    abort()s.
 * The first draw is dropped and fails on its readback like any wrong result; the
 * second goes out with alpha-to-coverage off.  A geometry shader under tessellation
 * is not among them: the one D3DMetal cannot compile there is the pass-through GS,
 * which is never bound (gs_tagged_pass_through), and every other one draws.  The
 * context tracks just enough state to see each coming.
 */
#define TESS_PATCH_BUDGET 0x3b80

static void ctx_forget_state(dm_context *w)
{
    w->gs_bound = w->hs_bound = w->ds_bound = 0;
    w->vs = w->vs_sized = NULL;
    w->vs_bytes = 0;
    w->n_elems = 0;
    memset(w->vb, 0, sizeof(w->vb));
    w->ib = NULL;
    w->ib_format = DXGI_FORMAT_UNKNOWN;
    w->ib_offset = w->ib_size = 0;
    w->patch_cps = 0;
    w->blend = w->blend_checked = w->blend_twin = NULL;
    w->rt0_unblendable = 0;
    w->rgb32_slots = 0;
}

static void ctx_note_layout(dm_context *w, void *il)
{
    UINT bytes = sizeof(w->elems);

    w->n_elems = 0;
    if (il && SUCCEEDED(ID3D11DeviceChild_GetPrivateData((ID3D11DeviceChild *)il,
                                                          &k_layout_tag, &bytes, w->elems))
        && bytes && bytes % sizeof(layout_elem) == 0)
        w->n_elems = bytes / sizeof(layout_elem);
}

static void ctx_note_vbs(dm_context *w, UINT start, UINT n, ID3D11Buffer *const *bufs,
                         const UINT *strides, const UINT *offsets)
{
    UINT i;

    for (i = 0; i < n && start + i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
    {
        bound_vb *v = &w->vb[start + i];

        v->buf = bufs ? bufs[i] : NULL;
        v->stride = strides ? strides[i] : 0;
        v->offset = offsets ? offsets[i] : 0;
        v->size = 0;
        if (v->buf)
        {
            D3D11_BUFFER_DESC d;

            ID3D11Buffer_GetDesc(v->buf, &d);
            v->size = d.ByteWidth;
        }
    }
}

static void ctx_note_ib(dm_context *w, ID3D11Buffer *ib, DXGI_FORMAT format, UINT offset)
{
    w->ib = ib;
    w->ib_format = format;
    w->ib_offset = offset;
    w->ib_size = 0;
    if (ib)
    {
        D3D11_BUFFER_DESC d;

        ID3D11Buffer_GetDesc(ib, &d);
        w->ib_size = d.ByteWidth;
    }
}

static void ctx_note_topology(dm_context *w, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    w->patch_cps = topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
                   && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST
                       ? topology - D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1
                       : 0;
}

static void ctx_note_blend(dm_context *w, ID3D11BlendState *bs, const FLOAT *factor,
                           UINT sample_mask)
{
    int i;

    w->blend = bs;
    w->blend_checked = NULL;
    w->blend_twin = NULL;
    for (i = 0; i < 4; i++)
        w->factor[i] = factor ? factor[i] : 1.f;
    w->sample_mask = sample_mask;
}

static void ctx_note_targets(dm_context *w, UINT n, ID3D11RenderTargetView *const *rtvs)
{
    D3D11_RENDER_TARGET_VIEW_DESC d;
    UINT i;

    w->rt0_unblendable = 0;
    w->rgb32_slots = 0;
    if (dm_knob_rgb32())
        for (i = 0; rtvs && i < n && i < 8; i++)
            if (rgb32_view((ID3D11View *)rtvs[i]))
                w->rgb32_slots |= 1u << i;
    if (!n || !rtvs || !rtvs[0])
        return;
    ID3D11RenderTargetView_GetDesc(rtvs[0], &d);
    w->rt0_unblendable = !dm_format_blendable(d.Format);
}

/* The state the guards track, read back from the real context: after
 * SwapDeviceContextState the incoming state object carries its own bindings. */
static void ctx_refresh_state(dm_context *w)
{
    ID3D11DeviceContext *c = dxcts_real(w);
    void *shader = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *bufs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11Buffer *ib = NULL;
    DXGI_FORMAT ib_format;
    UINT ib_offset, i;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11BlendState *bs = NULL;
    FLOAT factor[4];
    UINT mask;
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv = NULL;

    ctx_forget_state(w);
    ID3D11DeviceContext_GSGetShader(c, (ID3D11GeometryShader **)&shader, NULL, NULL);
    w->gs_bound = shader != NULL; release(shader); shader = NULL;
    ID3D11DeviceContext_HSGetShader(c, (ID3D11HullShader **)&shader, NULL, NULL);
    w->hs_bound = shader != NULL; release(shader); shader = NULL;
    ID3D11DeviceContext_DSGetShader(c, (ID3D11DomainShader **)&shader, NULL, NULL);
    w->ds_bound = shader != NULL; release(shader); shader = NULL;
    ID3D11DeviceContext_VSGetShader(c, (ID3D11VertexShader **)&shader, NULL, NULL);
    w->vs = shader; release(shader);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ctx_note_layout(w, il);
    release(il);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
                                           bufs, strides, offsets);
    ctx_note_vbs(w, 0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, bufs, strides, offsets);
    for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
        release(bufs[i]);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &ib_format, &ib_offset);
    ctx_note_ib(w, ib, ib_format, ib_offset);
    release(ib);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &topo);
    ctx_note_topology(w, topo);
    ID3D11DeviceContext_OMGetBlendState(c, &bs, factor, &mask);
    ctx_note_blend(w, bs, factor, mask);
    release(bs);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs,
                                           &dsv);
    ctx_note_targets(w, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs);
    for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        release(rtvs[i]);
    release(dsv);
}

/* The alpha-to-coverage-free twin of `bs`, or NULL when `bs` does not enable
 * alpha-to-coverage.  Made once per state and kept by it. */
static ID3D11BlendState *blend_without_a2c(ID3D11Device *dev, ID3D11BlendState *bs)
{
    ID3D11BlendState *twin = NULL;
    UINT size = sizeof(twin);
    D3D11_BLEND_DESC d;

    if (SUCCEEDED(ID3D11BlendState_GetPrivateData(bs, &k_blend_no_a2c_ptr_tag, &size, &twin))
        && size == sizeof(twin))
        return twin;
    ID3D11BlendState_GetDesc(bs, &d);
    if (!d.AlphaToCoverageEnable)
        return NULL;
    d.AlphaToCoverageEnable = FALSE;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &d, &twin)) || !twin)
        return NULL;
    if (SUCCEEDED(ID3D11BlendState_SetPrivateDataInterface(bs, &k_blend_no_a2c_tag,
                                                           (IUnknown *)twin)))
        release(twin); /* the state's slot holds the surviving reference */
    ID3D11BlendState_SetPrivateData(bs, &k_blend_no_a2c_ptr_tag, sizeof(twin), &twin);
    return twin;
}

/* The twins made for a blend state (or, for the null state, for the device), kept on
 * it: key = the twin slots and whether alpha-to-coverage was dropped. */
typedef struct
{
    UINT key[4];
    ID3D11BlendState *twin[4];
    UINT n;
} mask_twins;

static D3D11_BLEND fold_dest_alpha(D3D11_BLEND f)
{
    switch (f)
    {
    case D3D11_BLEND_DEST_ALPHA:     return D3D11_BLEND_ONE;
    case D3D11_BLEND_INV_DEST_ALPHA: return D3D11_BLEND_ZERO;
    case D3D11_BLEND_SRC_ALPHA_SAT:  return D3D11_BLEND_ZERO;
    default:                         return f;
    }
}

/* The twin of `bs` (NULL = the default blend state) whose blends on the R32G32B32 twin
 * slots in `rgb32_slots` read the destination alpha as 1, the way a 3-channel target
 * reads; alpha-to-coverage is dropped as well when `drop_a2c`.  One reference for the
 * caller. */
static ID3D11BlendState *blend_with_alpha_masked(ID3D11Device *dev, ID3D11BlendState *bs,
                                                 UINT rgb32_slots, int drop_a2c)
{
    ID3D11DeviceChild *holder = (ID3D11DeviceChild *)bs;
    UINT key = rgb32_slots | (drop_a2c ? 0x100u : 0u);
    mask_twins cache;
    UINT size = sizeof(cache), i;
    D3D11_BLEND_DESC d;
    ID3D11BlendState *twin = NULL;
    int have;

    memset(&cache, 0, sizeof(cache));
    have = holder ? SUCCEEDED(ID3D11DeviceChild_GetPrivateData(holder, &k_mask_twins_tag, &size,
                                                               &cache))
                  : SUCCEEDED(ID3D11Device_GetPrivateData(dev, &k_mask_twins_tag, &size,
                                                          &cache));
    have = have && size == sizeof(cache);
    if (have)
        for (i = 0; i < cache.n; i++)
            if (cache.key[i] == key)
            {
                addref(cache.twin[i]);
                return cache.twin[i];
            }
    memset(&d, 0, sizeof(d));
    if (bs)
        ID3D11BlendState_GetDesc(bs, &d);
    else
    {
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    if (drop_a2c)
        d.AlphaToCoverageEnable = FALSE;
    /* Slot 0 alone needs no per-slot state (and D3DMetal was seen to ignore the write
     * mask once IndependentBlendEnable is switched on). */
    if (!d.IndependentBlendEnable && rgb32_slots != 1)
    {
        d.IndependentBlendEnable = TRUE;
        for (i = 1; i < 8; i++)
            d.RenderTarget[i] = d.RenderTarget[0];
    }
    /* D3DMetal does not honour the write mask for the alpha channel, so the factors
     * that read the destination alpha are folded for the value a 3-channel target
     * reads (1). */
    for (i = 0; i < 8; i++)
        if (rgb32_slots & (1u << i))
        {
            D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[i];

            r->RenderTargetWriteMask &= ~(UINT8)D3D11_COLOR_WRITE_ENABLE_ALPHA;
            r->SrcBlend = fold_dest_alpha(r->SrcBlend);
            r->DestBlend = fold_dest_alpha(r->DestBlend);
            r->SrcBlendAlpha = fold_dest_alpha(r->SrcBlendAlpha);
            r->DestBlendAlpha = fold_dest_alpha(r->DestBlendAlpha);
        }
    if (FAILED(ID3D11Device_CreateBlendState(dev, &d, &twin)) || !twin)
        return NULL;
    if (!have)
        memset(&cache, 0, sizeof(cache));
    if (cache.n < 4)
    {
        cache.key[cache.n] = key;
        cache.twin[cache.n] = twin;
        cache.n++;
        addref(twin);   /* the cache's reference; the caller keeps the other */
        if (holder)
            ID3D11DeviceChild_SetPrivateData(holder, &k_mask_twins_tag, sizeof(cache), &cache);
        else
            ID3D11Device_SetPrivateData(dev, &k_mask_twins_tag, sizeof(cache), &cache);
    }
    return twin;
}

typedef struct
{
    int skip, swapped;
} draw_guard;

static void draw_guard_enter(dm_context *w, draw_guard *g)
{
    ID3D11DeviceContext *real = dxcts_real(w);
    ID3D11Device *dev = ctx_real_device(w);

    g->skip = 0;
    g->swapped = 0;
    if (dm_knob_draw_guards())
    {
        if (w->hs_bound && w->patch_cps)
        {
            uint64_t need;

            if (w->vs_sized != w->vs)
            {
                UINT bytes = 0, size = sizeof(bytes);

                w->vs_sized = w->vs;
                w->vs_bytes = 0;
                if (w->vs
                    && SUCCEEDED(ID3D11VertexShader_GetPrivateData(w->vs, &k_vs_bytes_tag,
                                                                   &size, &bytes))
                    && size == sizeof(bytes))
                    w->vs_bytes = bytes;
            }
            need = (uint64_t)w->vs_bytes * w->patch_cps;
            if (need > TESS_PATCH_BUDGET)
            {
                dm_count(&dm_counts.dropped_draws);
                g->skip = 1;
                return;
            }
        }
    }
    if (w->rgb32_slots)
    {
        int drop_a2c = dm_knob_draw_guards() && w->rt0_unblendable;
        UINT key = w->rgb32_slots | (drop_a2c ? 0x100u : 0u);

        if (!w->mask_twin || w->mask_twin_for != w->blend || w->mask_twin_key != key)
        {
            release(w->mask_twin);
            w->mask_twin = blend_with_alpha_masked(dev, w->blend, w->rgb32_slots, drop_a2c);
            w->mask_twin_for = w->blend;
            w->mask_twin_key = key;
        }
        if (w->mask_twin)
        {
            ID3D11DeviceContext_OMSetBlendState(real, w->mask_twin, w->factor, w->sample_mask);
            g->swapped = 1;
        }
        return;
    }
    if (dm_knob_draw_guards() && w->rt0_unblendable && w->blend)
    {
        if (w->blend_checked != w->blend)
        {
            w->blend_checked = w->blend;
            w->blend_twin = blend_without_a2c(dev, w->blend);
        }
        if (w->blend_twin)
        {
            dm_count(&dm_counts.altered_draws);
            ID3D11DeviceContext_OMSetBlendState(real, w->blend_twin, w->factor, w->sample_mask);
            g->swapped = 1;
        }
    }
}

static void draw_guard_leave(dm_context *w, draw_guard *g)
{
    if (g->swapped)
        ID3D11DeviceContext_OMSetBlendState((ID3D11DeviceContext *)dxcts_real(w), w->blend, w->factor,
                                            w->sample_mask);
}

/* --- fetch bounds --- */
/*
 * D3D reads a vertex element that does not lie entirely inside its buffer as zeros,
 * and an index past the end of the index buffer as 0.  D3DMetal checks neither: the
 * fetch reads whatever follows the allocation.  Before a draw whose reach exceeds a
 * bound buffer, that buffer is replaced for the draw by a zero-filled copy long enough
 * for every fetch, holding the source up to the first read that would straddle its
 * end.  Per-vertex reach is known for the non-indexed draws only (an indexed draw's
 * vertex indices live on the GPU); per-instance reach and the index buffer's are known
 * for all.  An indirect draw's arguments are read back from the GPU first, but only
 * when a bound element's very first fetch already lies outside its buffer (a binding
 * past the buffer's end): that is the one shape where the read cannot land in the
 * allocation's own padding, and the readback stalls.  Draws that would need more than
 * FETCH_BOUNDS_MAX_BYTES are left alone.
 */
#define FETCH_BOUNDS_MAX_BYTES (64u << 20)

typedef struct
{
    struct { UINT slot; ID3D11Buffer *buf; UINT stride, offset; } vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT n_vb;
    ID3D11Buffer *ib;
    DXGI_FORMAT ib_format;
    UINT ib_offset;
    int ib_swapped;
    ID3D11Buffer *temps[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT + 1];
    UINT n_temps;
} fetch_bounds;

/* The device's zero buffer of FETCH_BOUNDS_MAX_BYTES, made once and kept by the
 * device, so a padded copy is two GPU copies rather than a CPU-filled upload per draw. */
static ID3D11Buffer *device_zero_buffer(ID3D11Device *dev)
{
    ID3D11Buffer *zb = NULL;
    UINT size = sizeof(zb);
    unsigned char *zeros;
    D3D11_BUFFER_DESC d;
    D3D11_SUBRESOURCE_DATA init;

    if (SUCCEEDED(ID3D11Device_GetPrivateData(dev, &k_zero_buffer_tag, &size, &zb))
        && size == sizeof(zb) && zb)
        return zb;
    zeros = calloc(1, FETCH_BOUNDS_MAX_BYTES);
    if (!zeros)
        return NULL;
    memset(&d, 0, sizeof(d));
    d.ByteWidth = FETCH_BOUNDS_MAX_BYTES;
    d.Usage = D3D11_USAGE_DEFAULT;
    init.pSysMem = zeros; init.SysMemPitch = 0; init.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, &init, &zb)) || !zb)
        zb = NULL;
    free(zeros);
    if (!zb)
        return NULL;
    if (SUCCEEDED(ID3D11Device_SetPrivateDataInterface(dev, &k_zero_buffer_tag, (IUnknown *)zb)))
        release(zb); /* the device's slot holds the surviving reference */
    return zb;
}

/* A zero-filled buffer of `bytes` holding src[0, keep). */
static ID3D11Buffer *zero_padded_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                      ID3D11Buffer *src, uint64_t keep, uint64_t bytes,
                                      UINT bind)
{
    ID3D11Buffer *zb = device_zero_buffer(dev), *tmp = NULL;
    D3D11_BUFFER_DESC d;
    D3D11_BOX box;

    memset(&d, 0, sizeof(d));
    d.ByteWidth = (UINT)bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    if (!zb || FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &tmp)) || !tmp)
    {
        release(tmp);
        return NULL;
    }
    if (bytes > keep)
    {
        box.left = (UINT)keep; box.top = 0; box.front = 0;
        box.right = (UINT)bytes; box.bottom = 1; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmp, 0, (UINT)keep,
                                                  0, 0, (ID3D11Resource *)zb, 0, &box);
    }
    if (keep)
    {
        box.left = 0; box.top = 0; box.front = 0;
        box.right = (UINT)keep; box.bottom = 1; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmp, 0, 0, 0, 0,
                                                  (ID3D11Resource *)src, 0, &box);
    }
    return tmp;
}

/* `indexed`: per-vertex elements are not checked.  Vertex range [start_vertex,
 * +vertex_count), instance range [start_instance, +instances). */
static void fetch_bounds_enter(dm_context *w, fetch_bounds *fb, int indexed,
                               uint64_t start_vertex, uint64_t vertex_count,
                               uint64_t start_instance, uint64_t instances,
                               uint64_t start_index, uint64_t index_count)
{
    ID3D11DeviceContext *ctx = dxcts_real(w);
    ID3D11Device *dev = ctx_real_device(w);
    UINT slot, i;

    memset(fb, 0, sizeof(*fb));
    if (!dm_knob_fetch_bounds())
        return;
    if (indexed && w->ib && w->ib_size)
    {
        uint64_t isz = w->ib_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
        uint64_t reach = w->ib_offset + (start_index + index_count) * isz;

        if (reach > w->ib_size && reach <= FETCH_BOUNDS_MAX_BYTES)
        {
            uint64_t keep = w->ib_size;
            ID3D11Buffer *tmp;

            if (w->ib_offset < w->ib_size)
                keep = w->ib_offset + ((w->ib_size - w->ib_offset) / isz) * isz;
            tmp = zero_padded_copy(dev, ctx, w->ib, keep, reach, D3D11_BIND_INDEX_BUFFER);
            if (tmp)
            {
                fb->ib = w->ib;
                fb->ib_format = w->ib_format;
                fb->ib_offset = w->ib_offset;
                fb->ib_swapped = 1;
                fb->temps[fb->n_temps++] = tmp;
                ID3D11DeviceContext_IASetIndexBuffer(ctx, tmp, w->ib_format, w->ib_offset);
            }
        }
    }
    if (!w->n_elems || !vertex_count || !instances)
        return;
    for (slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; slot++)
    {
        const bound_vb *v = &w->vb[slot];
        uint64_t reach = 0, cut;
        ID3D11Buffer *tmp;

        if (!v->buf || !v->size)
            continue;
        cut = v->size;
        for (i = 0; i < w->n_elems; i++)
        {
            const layout_elem *e = &w->elems[i];
            uint64_t first, last, end, idx, start;

            if (e->slot != slot || !e->size || (indexed && !e->per_instance))
                continue;
            if (e->per_instance)
            {
                /* Step rate 0: the same element for every instance. */
                first = start_instance;
                last = e->step ? start_instance + (instances - 1) / e->step : start_instance;
            }
            else
            {
                first = start_vertex;
                last = start_vertex + vertex_count - 1;
            }
            end = v->offset + (uint64_t)v->stride * last + e->offset + e->size;
            if (end > reach)
                reach = end;
            if (end <= v->size)
                continue;
            /* First read of this element that does not fit: its start is where the
             * copy must stop. */
            idx = first;
            if (v->stride)
            {
                uint64_t base = v->offset + e->offset + e->size;

                if (v->size >= base)
                    idx = first > (v->size - base) / v->stride + 1
                              ? first : (v->size - base) / v->stride + 1;
            }
            start = v->offset + (uint64_t)v->stride * idx + e->offset;
            if (start < cut)
                cut = start;
        }
        if (reach <= v->size || reach > FETCH_BOUNDS_MAX_BYTES)
            continue;
        tmp = zero_padded_copy(dev, ctx, v->buf, cut, reach, D3D11_BIND_VERTEX_BUFFER);
        if (!tmp)
            continue;
        fb->vb[fb->n_vb].slot = slot;
        fb->vb[fb->n_vb].buf = v->buf;
        fb->vb[fb->n_vb].stride = v->stride;
        fb->vb[fb->n_vb].offset = v->offset;
        fb->n_vb++;
        fb->temps[fb->n_temps++] = tmp;
        ID3D11DeviceContext_IASetVertexBuffers(ctx, slot, 1, &tmp, &v->stride, &v->offset);
    }
    if (fb->n_temps)
        dm_count(&dm_counts.padded_draws);
}

static void fetch_bounds_leave(dm_context *w, fetch_bounds *fb)
{
    ID3D11DeviceContext *ctx = dxcts_real(w);
    UINT i;

    for (i = 0; i < fb->n_vb; i++)
        ID3D11DeviceContext_IASetVertexBuffers(ctx, fb->vb[i].slot, 1, &fb->vb[i].buf,
                                               &fb->vb[i].stride, &fb->vb[i].offset);
    if (fb->ib_swapped)
        ID3D11DeviceContext_IASetIndexBuffer(ctx, fb->ib, fb->ib_format, fb->ib_offset);
    for (i = 0; i < fb->n_temps; i++)
        release(fb->temps[i]);
}

/* 1 when some element the draw fetches lies outside its buffer from the very first
 * vertex or instance on. */
static int fetch_bounds_first_read_outside(dm_context *w, int indexed)
{
    UINT i;

    for (i = 0; i < w->n_elems; i++)
    {
        const layout_elem *e = &w->elems[i];
        const bound_vb *v = &w->vb[e->slot];

        if (!e->size || (indexed && !e->per_instance))
            continue;
        if (v->buf && v->size && (uint64_t)v->offset + e->offset + e->size > v->size)
            return 1;
    }
    return 0;
}

/* Read `n` uints of an indirect draw's arguments back from the GPU. */
static int read_indirect_args(dm_context *w, ID3D11Buffer *args, UINT offset, UINT *out,
                              UINT n)
{
    ID3D11DeviceContext *ctx = dxcts_real(w);
    ID3D11Device *dev = ctx_real_device(w);
    D3D11_BUFFER_DESC ad, d;
    ID3D11Buffer *stg = NULL;
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = 0;

    if (!args)
        return 0;
    ID3D11Buffer_GetDesc(args, &ad);
    if ((uint64_t)offset + n * 4 > ad.ByteWidth)
        return 0;
    memset(&d, 0, sizeof(d));
    d.ByteWidth = n * 4;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &stg)) || !stg)
        return 0;
    box.left = offset; box.top = 0; box.front = 0;
    box.right = offset + n * 4; box.bottom = 1; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)stg, 0, 0, 0, 0,
                                              (ID3D11Resource *)args, 0, &box);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))
        && m.pData)
    {
        memcpy(out, m.pData, n * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
        ok = 1;
    }
    release(stg);
    return ok;
}

/* --- the draws --- */

#define DRAW_PROLOGUE(w, g)                                                   \
    draw_guard g;                                                             \
    draw_guard_enter(w, &g);                                                  \
    if (g.skip)                                                               \
        return

void WINAPI ctx_draw(void *self, UINT vertex_count, UINT start_vertex)
{
    dm_context *w = self;
    fetch_bounds fb;
    DRAW_PROLOGUE(w, g);

    fetch_bounds_enter(w, &fb, 0, start_vertex, vertex_count, 0, 1, 0, 0);
    ((draw_fn)REAL_CTX(self, Draw))(dxcts_real(self), vertex_count, start_vertex);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_indexed(void *self, UINT index_count, UINT start_index,
                             INT base_vertex)
{
    dm_context *w = self;
    fetch_bounds fb;
    DRAW_PROLOGUE(w, g);

    fetch_bounds_enter(w, &fb, 1, 0, 1, 0, 1, start_index, index_count);
    ((draw_indexed_fn)REAL_CTX(self, DrawIndexed))(dxcts_real(self), index_count, start_index,
                                                   base_vertex);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_instanced(void *self, UINT vertex_count, UINT instance_count,
                               UINT start_vertex, UINT start_instance)
{
    dm_context *w = self;
    fetch_bounds fb;
    DRAW_PROLOGUE(w, g);

    fetch_bounds_enter(w, &fb, 0, start_vertex, vertex_count, start_instance,
                       instance_count, 0, 0);
    ((draw_instanced_fn)REAL_CTX(self, DrawInstanced))(dxcts_real(self), vertex_count,
                                                       instance_count, start_vertex,
                                                       start_instance);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_indexed_instanced(void *self, UINT index_count,
                                       UINT instance_count, UINT start_index,
                                       INT base_vertex, UINT start_instance)
{
    dm_context *w = self;
    fetch_bounds fb;
    DRAW_PROLOGUE(w, g);

    fetch_bounds_enter(w, &fb, 1, 0, 1, start_instance, instance_count, start_index,
                       index_count);
    ((draw_indexed_instanced_fn)REAL_CTX(self, DrawIndexedInstanced))(
        dxcts_real(self), index_count, instance_count, start_index, base_vertex,
        start_instance);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_auto(void *self)
{
    dm_context *w = self;
    DRAW_PROLOGUE(w, g);

    ((void_fn)REAL_CTX(self, DrawAuto))(dxcts_real(self));
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_instanced_indirect(void *self, ID3D11Buffer *args, UINT offset)
{
    dm_context *w = self;
    fetch_bounds fb;
    UINT a[4];
    DRAW_PROLOGUE(w, g);

    memset(&fb, 0, sizeof(fb));
    if (dm_knob_fetch_bounds() && fetch_bounds_first_read_outside(w, 0)
        && read_indirect_args(w, args, offset, a, 4)
        && (uint64_t)a[2] + a[0] <= 0xffffffffu && (uint64_t)a[3] + a[1] <= 0xffffffffu)
        fetch_bounds_enter(w, &fb, 0, a[2], a[0], a[3], a[1], 0, 0);
    ((draw_indirect_fn)REAL_CTX(self, DrawInstancedIndirect))(dxcts_real(self), args, offset);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

void WINAPI ctx_draw_indexed_instanced_indirect(void *self, ID3D11Buffer *args,
                                                UINT offset)
{
    dm_context *w = self;
    fetch_bounds fb;
    UINT a[5];
    DRAW_PROLOGUE(w, g);

    memset(&fb, 0, sizeof(fb));
    if (dm_knob_fetch_bounds() && fetch_bounds_first_read_outside(w, 1)
        && read_indirect_args(w, args, offset, a, 5)
        && (uint64_t)a[2] + a[0] <= 0xffffffffu && (uint64_t)a[4] + a[1] <= 0xffffffffu)
        fetch_bounds_enter(w, &fb, 1, 0, 1, a[4], a[1], a[2], a[0]);
    ((draw_indirect_fn)REAL_CTX(self, DrawIndexedInstancedIndirect))(dxcts_real(self), args,
                                                                     offset);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

/* --- binding hooks --- */

void WINAPI ctx_gs_set_shader(void *self, void *gs, void *const *inst, UINT n)
{
    dm_context *w = self;

    if (gs && dm_knob_draw_guards() && gs_tagged_pass_through(gs))
    {
        gs = NULL;
        inst = NULL;
        n = 0;
    }
    w->gs_bound = gs != NULL;
    ((set_shader_fn)REAL_CTX(self, GSSetShader))(dxcts_real(self), gs, inst, n);
}

void WINAPI ctx_hs_set_shader(void *self, void *hs, void *const *inst, UINT n)
{
    ((dm_context *)self)->hs_bound = hs != NULL;
    ((set_shader_fn)REAL_CTX(self, HSSetShader))(dxcts_real(self), hs, inst, n);
}

void WINAPI ctx_ds_set_shader(void *self, void *ds, void *const *inst, UINT n)
{
    ((dm_context *)self)->ds_bound = ds != NULL;
    ((set_shader_fn)REAL_CTX(self, DSSetShader))(dxcts_real(self), ds, inst, n);
}

void WINAPI ctx_vs_set_shader(void *self, void *vs, void *const *inst, UINT n)
{
    dm_context *w = self;

    w->vs = vs;
    w->vs_sized = NULL;
    ((set_shader_fn)REAL_CTX(self, VSSetShader))(dxcts_real(self), vs, inst, n);
}

void WINAPI ctx_ia_set_topology(void *self, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    ctx_note_topology(self, topology);
    ((set_topology_fn)REAL_CTX(self, IASetPrimitiveTopology))(dxcts_real(self), topology);
}

void WINAPI ctx_ia_set_layout(void *self, void *il)
{
    ctx_note_layout(self, il);
    ((set_layout_fn)REAL_CTX(self, IASetInputLayout))(dxcts_real(self), il);
}

void WINAPI ctx_ia_set_vbs(void *self, UINT start, UINT n, ID3D11Buffer *const *bufs,
                           const UINT *strides, const UINT *offsets)
{
    ctx_note_vbs(self, start, n, bufs, strides, offsets);
    ((set_vbs_fn)REAL_CTX(self, IASetVertexBuffers))(dxcts_real(self), start, n, bufs,
                                                     strides, offsets);
}

void WINAPI ctx_ia_set_ib(void *self, ID3D11Buffer *ib, DXGI_FORMAT format, UINT offset)
{
    ctx_note_ib(self, ib, format, offset);
    ((set_ib_fn)REAL_CTX(self, IASetIndexBuffer))(dxcts_real(self), ib, format, offset);
}

void WINAPI ctx_om_set_blend(void *self, ID3D11BlendState *bs, const FLOAT *factor,
                             UINT sample_mask)
{
    ctx_note_blend(self, bs, factor, sample_mask);
    ((set_blend_fn)REAL_CTX(self, OMSetBlendState))(dxcts_real(self), bs, factor, sample_mask);
}

void WINAPI ctx_om_set_rts(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                           ID3D11DepthStencilView *dsv)
{
    ctx_note_targets(self, n, rtvs);
    ((set_rts_fn)REAL_CTX(self, OMSetRenderTargets))(dxcts_real(self), n, rtvs, dsv);
}

void WINAPI ctx_om_set_rts_uav(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                               ID3D11DepthStencilView *dsv, UINT uav_start,
                               UINT n_uavs, ID3D11UnorderedAccessView *const *uavs,
                               const UINT *counts)
{
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        ctx_note_targets(self, n, rtvs);
    ((set_rts_uav_fn)REAL_CTX(self, OMSetRenderTargetsAndUnorderedAccessViews))(
        dxcts_real(self), n, rtvs, dsv, uav_start, n_uavs, uavs, counts);
}

void WINAPI ctx_clear_state(void *self)
{
    ((void_fn)REAL_CTX(self, ClearState))(dxcts_real(self));
    ctx_forget_state(self);
}

void WINAPI ctx_execute_command_list(void *self, void *list, BOOL restore)
{
    ((exec_cmdlist_fn)REAL_CTX(self, ExecuteCommandList))(dxcts_real(self), list, restore);
    /* With RestoreContextState clear, replaying the list leaves the context in its
     * default state; with it set the bindings come back as they were. */
    if (!restore)
        ctx_forget_state(self);
}

HRESULT WINAPI ctx_finish_command_list(void *self, BOOL restore, void **list)
{
    HRESULT hr = ((finish_cmdlist_fn)REAL_CTX(self, FinishCommandList))(dxcts_real(self),
                                                                       restore, list);

    if (!restore)
        ctx_forget_state(self);
    return hr;
}

void WINAPI ctx_swap_state(void *self, void *state, void **previous)
{
    ((swap_state_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl,
                                                 SwapDeviceContextState)])(
        dxcts_real(self), state, previous);
    ctx_refresh_state(self);
}

