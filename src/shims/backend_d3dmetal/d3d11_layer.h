/*
 * The D3DMetal layer's D3D11 half, shared between its files: a wrapped device and
 * wrapped contexts (see common/comwrap.h) carrying the workarounds listed in
 * backend.c.  Everything the layer does for itself goes to the real objects directly,
 * never back through a wrapper, so a workaround's own draws and copies are not
 * re-examined by the others.
 */

#ifndef DXCTS_D3DMETAL_D3D11_LAYER_H
#define DXCTS_D3DMETAL_D3D11_LAYER_H

#include "d3dmetal.h"

#include "../../common/d3d11_ifaces.h"

#include <stdlib.h>

typedef HRESULT (WINAPI *check_msaa_fn)(void *, UINT, UINT, UINT *);
typedef HRESULT (WINAPI *check_msaa1_fn)(void *, UINT, UINT, UINT, UINT *);
typedef HRESULT (WINAPI *check_feature_fn)(void *, UINT, void *, UINT);
typedef HRESULT (WINAPI *check_format_fn)(void *, UINT, UINT *);
typedef void (WINAPI *get_context_fn)(void *, void **);
typedef HRESULT (WINAPI *create_deferred_fn)(void *, UINT, void **);
typedef HRESULT (WINAPI *create_texture_fn)(void *, const void *,
                                           const D3D11_SUBRESOURCE_DATA *, void **);
typedef HRESULT (WINAPI *create_view_fn)(void *, void *, const void *, void **);
typedef HRESULT (WINAPI *create_layout_fn)(void *, const D3D11_INPUT_ELEMENT_DESC *,
                                          UINT, const void *, SIZE_T, void **);
typedef HRESULT (WINAPI *create_gs_so_fn)(void *, const void *, SIZE_T,
                                         const D3D11_SO_DECLARATION_ENTRY *, UINT,
                                         const UINT *, UINT, UINT, void *, void **);
typedef HRESULT (WINAPI *create_shader_fn)(void *, const void *, SIZE_T, void *,
                                          void **);
typedef void (WINAPI *set_shader_fn)(void *, void *, void *const *, UINT);
typedef void (WINAPI *get_shader_fn)(void *, void **, void **, UINT *);
typedef void (WINAPI *set_topology_fn)(void *, D3D11_PRIMITIVE_TOPOLOGY);
typedef void (WINAPI *set_layout_fn)(void *, void *);
typedef void (WINAPI *set_vbs_fn)(void *, UINT, UINT, ID3D11Buffer *const *,
                                  const UINT *, const UINT *);
typedef void (WINAPI *set_ib_fn)(void *, ID3D11Buffer *, DXGI_FORMAT, UINT);
typedef void (WINAPI *set_blend_fn)(void *, ID3D11BlendState *, const FLOAT *, UINT);
typedef void (WINAPI *set_rts_fn)(void *, UINT, ID3D11RenderTargetView *const *,
                                  ID3D11DepthStencilView *);
typedef void (WINAPI *set_rts_uav_fn)(void *, UINT, ID3D11RenderTargetView *const *,
                                      ID3D11DepthStencilView *, UINT, UINT,
                                      ID3D11UnorderedAccessView *const *, const UINT *);
typedef void (WINAPI *void_fn)(void *);
typedef void (WINAPI *exec_cmdlist_fn)(void *, void *, BOOL);
typedef HRESULT (WINAPI *finish_cmdlist_fn)(void *, BOOL, void **);
typedef void (WINAPI *swap_state_fn)(void *, void *, void **);
typedef void (WINAPI *copy_region_fn)(void *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                      ID3D11Resource *, UINT, const D3D11_BOX *);
typedef void (WINAPI *copy_region1_fn)(void *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                       ID3D11Resource *, UINT, const D3D11_BOX *, UINT);
typedef void (WINAPI *copy_resource_fn)(void *, ID3D11Resource *, ID3D11Resource *);
typedef HRESULT (WINAPI *map_fn)(void *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                 D3D11_MAPPED_SUBRESOURCE *);
typedef void (WINAPI *unmap_fn)(void *, ID3D11Resource *, UINT);
typedef void (WINAPI *update_fn)(void *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                 const void *, UINT, UINT);
typedef void (WINAPI *update1_fn)(void *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                  const void *, UINT, UINT, UINT);
typedef void (WINAPI *resolve_fn)(void *, ID3D11Resource *, UINT, ID3D11Resource *,
                                  UINT, DXGI_FORMAT);
typedef void (WINAPI *clear_rtv_fn)(void *, ID3D11RenderTargetView *, const FLOAT *);
typedef void (WINAPI *draw_fn)(void *, UINT, UINT);
typedef void (WINAPI *draw_indexed_fn)(void *, UINT, UINT, INT);
typedef void (WINAPI *draw_instanced_fn)(void *, UINT, UINT, UINT, UINT);
typedef void (WINAPI *draw_indexed_instanced_fn)(void *, UINT, UINT, UINT, INT, UINT);
typedef void (WINAPI *draw_indirect_fn)(void *, ID3D11Buffer *, UINT);
typedef HRESULT (WINAPI *enqueue_fn)(void *, HANDLE);

#define REAL_DEV(self, method) \
    dxcts_real_vtable(self)[SLOT(ID3D11DeviceVtbl, method)]
#define REAL_CTX(self, method) \
    dxcts_real_vtable(self)[SLOT(ID3D11DeviceContextVtbl, method)]

/* --- private-data slots on the real objects --- */

/* {6d2c0d3a-8c1e-4b0f-9a6b-2f0c3e5d7a11}: an R32G32B32 twin's real format and alpha. */
static const GUID k_rgb32_tag =
    { 0x6d2c0d3a, 0x8c1e, 0x4b0f, { 0x9a, 0x6b, 0x2f, 0x0c, 0x3e, 0x5d, 0x7a, 0x11 } };
/* {7a91d3c4-2e60-4b8f-a157-3d9c6e02b471}: an input layout's elements. */
static const GUID k_layout_tag =
    { 0x7a91d3c4, 0x2e60, 0x4b8f, { 0xa1, 0x57, 0x3d, 0x9c, 0x6e, 0x02, 0xb4, 0x71 } };
/* {4b8e1c02-6f5d-4d7a-9e31-5c0a7f21d418}: a geometry shader that passes through. */
static const GUID k_passthrough_tag =
    { 0x4b8e1c02, 0x6f5d, 0x4d7a, { 0x9e, 0x31, 0x5c, 0x0a, 0x7f, 0x21, 0xd4, 0x18 } };
/* {5b0c2e7a-31d4-4f9b-a6e2-8c4d1f7b9e03}: a vertex shader's output bytes per vertex. */
static const GUID k_vs_bytes_tag =
    { 0x5b0c2e7a, 0x31d4, 0x4f9b, { 0xa6, 0xe2, 0x8c, 0x4d, 0x1f, 0x7b, 0x9e, 0x03 } };
/* {c2a7d9f1-6e3b-4b58-9d0a-2f81c6e4a7b5}: a blend state's alpha-to-coverage-free
 * twin, held as an interface so it dies with the state ... */
static const GUID k_blend_no_a2c_tag =
    { 0xc2a7d9f1, 0x6e3b, 0x4b58, { 0x9d, 0x0a, 0x2f, 0x81, 0xc6, 0xe4, 0xa7, 0xb5 } };
/* {c2a7d9f1-6e3b-4b58-9d0a-2f81c6e4a7b6}: ... and its bare pointer. */
static const GUID k_blend_no_a2c_ptr_tag =
    { 0xc2a7d9f1, 0x6e3b, 0x4b58, { 0x9d, 0x0a, 0x2f, 0x81, 0xc6, 0xe4, 0xa7, 0xb6 } };
/* {5c0e9b2a-3f7d-4a61-8e2b-6d4c1f9a0b37}: the alpha-masked twins of a blend state. */
static const GUID k_mask_twins_tag =
    { 0x5c0e9b2a, 0x3f7d, 0x4a61, { 0x8e, 0x2b, 0x6d, 0x4c, 0x1f, 0x9a, 0x0b, 0x37 } };
/* {b7e1c4d2-5a3f-4e8b-9c0d-1f2a3b4c5d6e}: API-layout bytes of a staging depth-stencil
 * texture, as last written through Map. */
static const GUID k_ds_shadow_tag =
    { 0xb7e1c4d2, 0x5a3f, 0x4e8b, { 0x9c, 0x0d, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e } };
/* {2f6b8c1e-7d4a-4c93-b0e5-9a1d3c7f2e60}: the device's zero buffer. */
static const GUID k_zero_buffer_tag =
    { 0x2f6b8c1e, 0x7d4a, 0x4c93, { 0xb0, 0xe5, 0x9a, 0x1d, 0x3c, 0x7f, 0x2e, 0x60 } };


/* --- the wrappers --- */

/* The D3D11CreateDevice arguments a device was made with: the key a parked device is
 * served back under. */
typedef struct
{
    uint64_t luid;
    UINT driver_type, flags, sdk, nlevels;
    UINT levels[16];
} dev_key;

typedef struct
{
    dxcts_wrapper base;
    int reusable;
    dev_key key;
    UINT level;
    DWORD context_thread;
    LONG creates_since_drain;
    /* The stencil-upload pipeline, made on first use; tried says whether it was. */
    int stencil_tried;
    ID3D11VertexShader *st_vs;
    ID3D11InputLayout *st_il;
    ID3D11DepthStencilState *st_dss;
    ID3D11RasterizerState *st_rs;
} dm_device;

typedef struct
{
    UINT slot, offset, size, per_instance, step;
} layout_elem;

typedef struct
{
    ID3D11Buffer *buf;
    UINT stride, offset, size;
} bound_vb;

typedef struct
{
    dxcts_wrapper base;
    dm_device *device;          /* held: a context outlives its device otherwise */
    int gs_bound, hs_bound, ds_bound;
    ID3D11VertexShader *vs, *vs_sized;
    UINT vs_bytes;
    layout_elem elems[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT n_elems;
    bound_vb vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11Buffer *ib;
    DXGI_FORMAT ib_format;
    UINT ib_offset, ib_size;
    UINT patch_cps;             /* control points of a bound patch list, else 0 */
    ID3D11BlendState *blend, *blend_checked, *blend_twin;
    UINT rgb32_slots;           /* bound target slots that are R32G32B32 twins */
    ID3D11BlendState *mask_twin, *mask_twin_for;
    UINT mask_twin_key;
    FLOAT factor[4];
    UINT sample_mask;
    int rt0_unblendable;
} dm_context;

/* --- the device and context wrappers (d3d11.c) --- */

void lock(void);
void unlock(void);

/* --- R32G32B32 twins, depth-stencil uploads and copies (d3d11_resources.c) --- */

int rgb32_view(ID3D11View *view);
ID3D11Device *ctx_real_device(dm_context *w);
void WINAPI ctx_copy_resource(void *self, ID3D11Resource *dst, ID3D11Resource *src);
void WINAPI ctx_copy_region(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
                            UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                            const D3D11_BOX *box);
void WINAPI ctx_copy_region1(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
                             UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                             const D3D11_BOX *box, UINT flags);
void WINAPI ctx_update(void *self, ID3D11Resource *dst, UINT sub,
                       const D3D11_BOX *box, const void *src, UINT src_row,
                       UINT src_depth);
void WINAPI ctx_update1(void *self, ID3D11Resource *dst, UINT sub,
                        const D3D11_BOX *box, const void *src, UINT src_row,
                        UINT src_depth, UINT flags);
HRESULT WINAPI ctx_map(void *self, ID3D11Resource *res, UINT sub, D3D11_MAP type,
                       UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped);
void WINAPI ctx_unmap(void *self, ID3D11Resource *res, UINT sub);
void WINAPI ctx_resolve(void *self, ID3D11Resource *dst, UINT dst_sub,
                        ID3D11Resource *src, UINT src_sub, DXGI_FORMAT format);
void WINAPI ctx_clear_rtv(void *self, ID3D11RenderTargetView *rtv, const FLOAT color[4]);
void WINAPI ctx_discard_resource(void *self, ID3D11Resource *res);
void WINAPI ctx_set_min_lod(void *self, ID3D11Resource *res, FLOAT lod);
FLOAT WINAPI ctx_get_min_lod(void *self, ID3D11Resource *res);
HRESULT WINAPI dev_create_dsv(void *self, ID3D11Resource *res,
                              const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, void **out);
HRESULT WINAPI dev_create_tex1d(void *self, const D3D11_TEXTURE1D_DESC *desc,
                                const D3D11_SUBRESOURCE_DATA *init, void **out);
HRESULT WINAPI dev_create_tex2d(void *self, const D3D11_TEXTURE2D_DESC *desc,
                                const D3D11_SUBRESOURCE_DATA *init, void **out);
HRESULT WINAPI dev_create_tex3d(void *self, const D3D11_TEXTURE3D_DESC *desc,
                                const D3D11_SUBRESOURCE_DATA *init, void **out);
HRESULT WINAPI dev_create_tex2d1(void *self, const D3D11_TEXTURE2D_DESC1 *desc,
                                 const D3D11_SUBRESOURCE_DATA *init, void **out);
HRESULT WINAPI dev_create_tex3d1(void *self, const D3D11_TEXTURE3D_DESC1 *desc,
                                 const D3D11_SUBRESOURCE_DATA *init, void **out);

/* --- shader bytecode (dxbc.c) --- */

int gs_is_pass_through(const void *code, size_t len);
int gs_tagged_pass_through(void *gs);
UINT vertex_output_bytes(const void *code, size_t len);
int ps_minprec_on_position(const void *code, size_t size);

/* --- draw-time guards (d3d11_draw.c) --- */

void WINAPI ctx_draw(void *self, UINT vertex_count, UINT start_vertex);
void WINAPI ctx_draw_indexed(void *self, UINT index_count, UINT start_index,
                             INT base_vertex);
void WINAPI ctx_draw_instanced(void *self, UINT vertex_count, UINT instance_count,
                               UINT start_vertex, UINT start_instance);
void WINAPI ctx_draw_indexed_instanced(void *self, UINT index_count,
                                       UINT instance_count, UINT start_index,
                                       INT base_vertex, UINT start_instance);
void WINAPI ctx_draw_auto(void *self);
void WINAPI ctx_draw_instanced_indirect(void *self, ID3D11Buffer *args, UINT offset);
void WINAPI ctx_draw_indexed_instanced_indirect(void *self, ID3D11Buffer *args,
                                                UINT offset);
void WINAPI ctx_gs_set_shader(void *self, void *gs, void *const *inst, UINT n);
void WINAPI ctx_hs_set_shader(void *self, void *hs, void *const *inst, UINT n);
void WINAPI ctx_ds_set_shader(void *self, void *ds, void *const *inst, UINT n);
void WINAPI ctx_vs_set_shader(void *self, void *vs, void *const *inst, UINT n);
void WINAPI ctx_ia_set_topology(void *self, D3D11_PRIMITIVE_TOPOLOGY topology);
void WINAPI ctx_ia_set_layout(void *self, void *il);
void WINAPI ctx_ia_set_vbs(void *self, UINT start, UINT n, ID3D11Buffer *const *bufs,
                           const UINT *strides, const UINT *offsets);
void WINAPI ctx_ia_set_ib(void *self, ID3D11Buffer *ib, DXGI_FORMAT format, UINT offset);
void WINAPI ctx_om_set_blend(void *self, ID3D11BlendState *bs, const FLOAT *factor,
                             UINT sample_mask);
void WINAPI ctx_om_set_rts(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                           ID3D11DepthStencilView *dsv);
void WINAPI ctx_om_set_rts_uav(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                               ID3D11DepthStencilView *dsv, UINT uav_start,
                               UINT n_uavs, ID3D11UnorderedAccessView *const *uavs,
                               const UINT *counts);
void WINAPI ctx_clear_state(void *self);
void WINAPI ctx_execute_command_list(void *self, void *list, BOOL restore);
HRESULT WINAPI ctx_finish_command_list(void *self, BOOL restore, void **list);
void WINAPI ctx_swap_state(void *self, void *state, void **previous);

HRESULT WINAPI dev_create_rtv(void *self, ID3D11Resource *res,
                              const D3D11_RENDER_TARGET_VIEW_DESC *desc, void **out);
HRESULT WINAPI dev_create_rtv1(void *self, ID3D11Resource *res,
                               const D3D11_RENDER_TARGET_VIEW_DESC1 *desc, void **out);
HRESULT WINAPI dev_create_srv(void *self, ID3D11Resource *res,
                              const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, void **out);
HRESULT WINAPI dev_create_srv1(void *self, ID3D11Resource *res,
                               const D3D11_SHADER_RESOURCE_VIEW_DESC1 *desc, void **out);
HRESULT WINAPI dev_create_uav(void *self, ID3D11Resource *res,
                              const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, void **out);
HRESULT WINAPI dev_create_uav1(void *self, ID3D11Resource *res,
                               const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *desc, void **out);

#endif
