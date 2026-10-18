/*
 * The D3DMetal layer's D3D11 half: a wrapped device and wrapped contexts (see
 * common/comwrap.h) carrying the workarounds listed in backend.c.  Everything the
 * layer does for itself goes to the real objects directly, never back through a
 * wrapper, so a workaround's own draws and copies are not re-examined by the others.
 */

#include "d3dmetal.h"

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

static void release(void *obj)
{
    if (obj)
        ((dxcts_ref_fn)(*(void ***)obj)[2])(obj);
}

static void addref(void *obj)
{
    if (obj)
        ((dxcts_ref_fn)(*(void ***)obj)[1])(obj);
}

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

static const GUID k_iid_d3d11_buffer =
    { 0x48570b85, 0xd1ee, 0x4fcd, { 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37 } };
static const GUID k_iid_dxgi_adapter =
    { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0 } };
static const GUID k_iid_dxgi_device2 =
    { 0x05008617, 0xfbfd, 0x4051, { 0xa7,0x90,0x14,0x48,0x84,0xb4,0xf6,0xa9 } };
static const GUID k_iid_dxgi_device3 =
    { 0x6007896c, 0x3244, 0x4afd, { 0xbf,0x18,0xa6,0xd3,0xbe,0xda,0x50,0x23 } };
static const GUID k_iid_dxgi_device4 =
    { 0x95b4f95f, 0xd8da, 0x4ca4, { 0x9e,0xe6,0x3b,0x76,0xd5,0x96,0x8a,0x10 } };
static const GUID k_iid_d3d11_device =
    { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40 } };
static const GUID k_iid_d3d11_device5 =
    { 0x8ffde202, 0xa0e7, 0x45df, { 0x9e,0x01,0xe8,0x37,0x80,0x1b,0x5e,0xa0 } };
static const GUID k_iid_d3d11_context4 =
    { 0x917600da, 0xf58c, 0x4c33, { 0x98,0xd8,0x3e,0x15,0xb3,0x90,0xfa,0x24 } };
static const GUID k_iid_d3d11_fence =
    { 0xaffde9d1, 0x1df7, 0x4bb7, { 0x8a,0x34,0x0f,0x46,0x25,0x1d,0xab,0x80 } };

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

static const dxcts_iface k_devices[] = {
    { { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40 } },
      WIDTH(ID3D11DeviceVtbl) },
    { { 0xa04bfb29, 0x08ef, 0x43d6, { 0xa4,0x9c,0xa9,0xbd,0xbd,0xcb,0xe6,0x86 } },
      WIDTH(ID3D11Device1Vtbl) },
    { { 0x9d06dffa, 0xd1e5, 0x4d07, { 0x83,0xa8,0x1b,0xb1,0x23,0xf2,0xf8,0x41 } },
      WIDTH(ID3D11Device2Vtbl) },
    { { 0xa05c8c37, 0xd2c6, 0x4732, { 0xb3,0xa0,0x9c,0xe0,0xb0,0xdc,0x9a,0xe6 } },
      WIDTH(ID3D11Device3Vtbl) },
    { { 0x8992ab71, 0x02e6, 0x4b8d, { 0xba,0x48,0xb0,0x56,0xdc,0xda,0x42,0xc4 } },
      WIDTH(ID3D11Device4Vtbl) },
    { { 0x8ffde202, 0xa0e7, 0x45df, { 0x9e,0x01,0xe8,0x37,0x80,0x1b,0x5e,0xa0 } },
      WIDTH(ID3D11Device5Vtbl) },
};

static const dxcts_iface k_contexts[] = {
    { { 0xc0bfa96c, 0xe089, 0x44fb, { 0x8e,0xaf,0x26,0xf8,0x79,0x61,0x90,0xda } },
      WIDTH(ID3D11DeviceContextVtbl) },
    { { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1 } },
      WIDTH(ID3D11DeviceContext1Vtbl) },
    { { 0x420d5b32, 0xb90c, 0x4da4, { 0xbe,0xf0,0x35,0x9f,0x6a,0x24,0xa8,0x3a } },
      WIDTH(ID3D11DeviceContext2Vtbl) },
    { { 0xb4e3c01d, 0xe79e, 0x4637, { 0x91,0xb2,0x51,0x0e,0x9f,0x4c,0x9b,0x8f } },
      WIDTH(ID3D11DeviceContext3Vtbl) },
    { { 0x917600da, 0xf58c, 0x4c33, { 0x98,0xd8,0x3e,0x15,0xb3,0x90,0xfa,0x24 } },
      WIDTH(ID3D11DeviceContext4Vtbl) },
};

static void *wrap_context(dm_device *device, void *ctx);

/* A lock over the layer's tables, kept simple: none of them is on a hot path. */
static CRITICAL_SECTION g_lock;
static LONG g_lock_ready;

static void lock_init(void)
{
    if (!g_lock_ready && InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lock_ready, 2);
    }
    while (g_lock_ready != 2)
        Sleep(0);
}

static void lock(void)
{
    lock_init();
    EnterCriticalSection(&g_lock);
}

static void unlock(void)
{
    LeaveCriticalSection(&g_lock);
}

/* --- R32G32B32 textures --- */
/*
 * Metal has no 96-bit pixel format.  D3DMetal backs the R32G32B32 formats with an
 * RGBA32 texture but keeps 12-byte texels on the CPU side, and every upload or copy it
 * issues for one is a blit with a 12-byte-per-texel bytes-per-row that AGX rejects
 * ("bytes_per_row >= used_bytes_per_row"): the texture is left zeroed, whether it was
 * created with initial data, filled with UpdateSubresource, or written through a
 * staging map.  So an R32G32B32 texture is created here as its RGBA32 twin, tagged
 * with its real format, and every CPU-visible byte path is repacked between the
 * 12-byte texels the API describes and the 16-byte texels the twin holds, with alpha
 * written as 1 the way a 3-channel format reads.  Views and resolves name the twin
 * format; copies between twins need nothing.
 */
typedef struct
{
    UINT orig_format, alpha_bits;
} rgb32_tag;

static int rgb32_twin(DXGI_FORMAT f, DXGI_FORMAT *twin, UINT *alpha)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        *twin = DXGI_FORMAT_R32G32B32A32_TYPELESS; *alpha = 0x3f800000u; return 1;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        *twin = DXGI_FORMAT_R32G32B32A32_FLOAT; *alpha = 0x3f800000u; return 1;
    case DXGI_FORMAT_R32G32B32_UINT:
        *twin = DXGI_FORMAT_R32G32B32A32_UINT; *alpha = 1u; return 1;
    case DXGI_FORMAT_R32G32B32_SINT:
        *twin = DXGI_FORMAT_R32G32B32A32_SINT; *alpha = 1u; return 1;
    default:
        return 0;
    }
}

/* Width, rows and slices of subresource `sub`, and the format, of any texture. */
typedef struct
{
    D3D11_RESOURCE_DIMENSION dim;
    DXGI_FORMAT format;
    UINT width, height, depth, mips, slices, bind, usage, cpu, samples, misc;
} tex_shape;

static int tex_shape_of(ID3D11Resource *r, tex_shape *t)
{
    memset(t, 0, sizeof(*t));
    if (!r)
        return 0;
    ID3D11Resource_GetType(r, &t->dim);
    if (t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
    {
        D3D11_TEXTURE1D_DESC d;
        ID3D11Texture1D_GetDesc((ID3D11Texture1D *)r, &d);
        t->format = d.Format; t->width = d.Width; t->height = 1; t->depth = 1;
        t->mips = d.MipLevels; t->slices = d.ArraySize; t->bind = d.BindFlags;
        t->usage = d.Usage; t->cpu = d.CPUAccessFlags; t->samples = 1; t->misc = d.MiscFlags;
    }
    else if (t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        t->format = d.Format; t->width = d.Width; t->height = d.Height; t->depth = 1;
        t->mips = d.MipLevels; t->slices = d.ArraySize; t->bind = d.BindFlags;
        t->usage = d.Usage; t->cpu = d.CPUAccessFlags; t->samples = d.SampleDesc.Count;
        t->misc = d.MiscFlags;
    }
    else if (t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC d;
        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)r, &d);
        t->format = d.Format; t->width = d.Width; t->height = d.Height;
        t->depth = d.Depth; t->mips = d.MipLevels; t->slices = 1; t->bind = d.BindFlags;
        t->usage = d.Usage; t->cpu = d.CPUAccessFlags; t->samples = 1; t->misc = d.MiscFlags;
    }
    else
        return 0;
    return t->mips != 0;
}

static UINT mip_dim(UINT v, UINT mip)
{
    v >>= mip;
    return v ? v : 1;
}

static int rgb32_tagged(ID3D11Resource *r, rgb32_tag *out)
{
    tex_shape t;
    rgb32_tag tag;
    UINT size = sizeof(tag);

    /* Only an RGBA32 twin can carry the tag, so every other resource is answered from
     * its description alone. */
    if (!tex_shape_of(r, &t) || t.format < DXGI_FORMAT_R32G32B32A32_TYPELESS
        || t.format > DXGI_FORMAT_R32G32B32A32_SINT)
        return 0;
    if (FAILED(ID3D11Resource_GetPrivateData(r, &k_rgb32_tag, &size, &tag))
        || size != sizeof(tag))
        return 0;
    if (out)
        *out = tag;
    return 1;
}

static int rgb32_view(ID3D11View *view)
{
    ID3D11Resource *res = NULL;
    int tagged;

    if (!view)
        return 0;
    ID3D11View_GetResource(view, &res);
    if (!res)
        return 0;
    tagged = rgb32_tagged(res, NULL);
    release(res);
    return tagged;
}

/* 12-byte texels -> 16-byte texels, rows x slices, alpha appended.  Texels are
 * copied as bytes: an application's initial data need not be 4-byte aligned. */
static void rgb32_pack(unsigned char *dst, size_t dst_row, size_t dst_slice,
                       const unsigned char *src, size_t src_row, size_t src_slice,
                       UINT w, UINT rows, UINT slices, UINT alpha)
{
    UINT x, y, z;

    for (z = 0; z < slices; z++)
        for (y = 0; y < rows; y++)
        {
            const unsigned char *s = src + z * src_slice + y * src_row;
            unsigned char *d = dst + z * dst_slice + y * dst_row;

            for (x = 0; x < w; x++)
            {
                memcpy(d + 16 * x, s + 12 * x, 12);
                memcpy(d + 16 * x + 12, &alpha, 4);
            }
        }
}

static void rgb32_unpack(unsigned char *dst, size_t dst_row, size_t dst_slice,
                         const unsigned char *src, size_t src_row, size_t src_slice,
                         UINT w, UINT rows, UINT slices)
{
    UINT x, y, z;

    for (z = 0; z < slices; z++)
        for (y = 0; y < rows; y++)
        {
            const unsigned char *s = src + z * src_slice + y * src_row;
            unsigned char *d = dst + z * dst_slice + y * dst_row;

            for (x = 0; x < w; x++)
                memcpy(d + 12 * x, s + 16 * x, 12);
        }
}

/* Extent of subresource `sub` of a tagged texture. */
static int rgb32_extent(ID3D11Resource *r, UINT sub, UINT *w, UINT *rows, UINT *slices)
{
    tex_shape t;
    UINT mip;

    if (!tex_shape_of(r, &t))
        return 0;
    mip = t.dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? sub : sub % t.mips;
    *w = mip_dim(t.width, mip);
    *rows = mip_dim(t.height, mip);
    *slices = mip_dim(t.depth, mip);
    return 1;
}

/* Initial data for a substituted create: one packed buffer per subresource. */
typedef struct
{
    unsigned char **bufs;
    D3D11_SUBRESOURCE_DATA *data;
    UINT n;
} rgb32_init;

static const D3D11_SUBRESOURCE_DATA *rgb32_pack_init(rgb32_init *out,
                                                     const D3D11_SUBRESOURCE_DATA *init,
                                                     UINT subs, UINT mips, UINT width,
                                                     UINT height, UINT depth, int is3d,
                                                     UINT alpha)
{
    UINT i;

    memset(out, 0, sizeof(*out));
    out->bufs = calloc(subs, sizeof(*out->bufs));
    out->data = calloc(subs, sizeof(*out->data));
    if (!out->bufs || !out->data)
        return init;
    out->n = subs;
    for (i = 0; i < subs; i++)
    {
        UINT mip = is3d ? i : (mips ? i % mips : 0);
        UINT w = mip_dim(width, mip), rows = mip_dim(height, mip);
        UINT slices = is3d ? mip_dim(depth, mip) : 1;
        size_t drow = (size_t)w * 16, dslice = drow * rows;
        size_t srow = init[i].SysMemPitch ? init[i].SysMemPitch : (size_t)w * 12;
        size_t sslice = init[i].SysMemSlicePitch ? init[i].SysMemSlicePitch : srow * rows;

        out->bufs[i] = malloc(dslice * slices);
        if (!out->bufs[i])
            return init;
        rgb32_pack(out->bufs[i], drow, dslice, init[i].pSysMem, srow, sslice, w, rows,
                   slices, alpha);
        out->data[i].pSysMem = out->bufs[i];
        out->data[i].SysMemPitch = (UINT)drow;
        out->data[i].SysMemSlicePitch = (UINT)dslice;
    }
    return out->data;
}

static void rgb32_init_free(rgb32_init *p)
{
    UINT i;

    for (i = 0; p->bufs && i < p->n; i++)
        free(p->bufs[i]);
    free(p->bufs);
    free(p->data);
}

static void rgb32_tag_set(void *child, DXGI_FORMAT orig, UINT alpha)
{
    rgb32_tag t = { (UINT)orig, alpha };

    ID3D11DeviceChild_SetPrivateData((ID3D11DeviceChild *)child, &k_rgb32_tag,
                                     sizeof(t), &t);
}

/*
 * The twin is handed out behind a wrapper whose GetDesc reports the format the caller
 * asked for: a test builds its readback staging texture from the render target's own
 * description, and one that read RGBA32 there would map 16-byte texels against the
 * 12-byte ones it expects.  Every other method thunks to the real texture.  The layer
 * unwraps at each entry point that takes a resource, so the implementation only ever
 * sees its own object; a view's GetResource still returns the real texture.
 */
typedef struct
{
    dxcts_wrapper base;
    DXGI_FORMAT orig;
} tex_wrapper;

typedef void (WINAPI *get_desc_fn)(void *, void *);

static HRESULT WINAPI tex_qi(void *self, const GUID *iid, void **out);

/* A wrapper of this layer's is recognised by its QueryInterface slot: reading slot 0
 * of any COM object's vtable is always safe, and no real object has that function. */
static ID3D11Resource *unwrap(void *res)
{
    if (res && (*(void ***)res)[0] == (void *)tex_qi)
        return dxcts_real(res);
    return res;
}

static const dxcts_iface k_textures1d[] = {
    { { 0xf8fb5c27, 0xc6b3, 0x4f75, { 0xa4,0xc8,0x43,0x9a,0xf2,0xef,0x56,0x4c } },
      WIDTH(ID3D11Texture1DVtbl) },
};
static const dxcts_iface k_textures2d[] = {
    { { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c } },
      WIDTH(ID3D11Texture2DVtbl) },
    { { 0x51218251, 0x1e33, 0x4617, { 0x9c,0xcb,0x4d,0x3a,0x43,0x67,0xe7,0xbb } },
      WIDTH(ID3D11Texture2D1Vtbl) },
};
static const dxcts_iface k_textures3d[] = {
    { { 0x037e866e, 0xf56d, 0x4357, { 0xa8,0xaf,0x9d,0xab,0xbe,0x6e,0x25,0x0e } },
      WIDTH(ID3D11Texture3DVtbl) },
    { { 0x0c711683, 0x2853, 0x4846, { 0x9b,0xb7,0xf3,0xe9,0x05,0x82,0xe8,0xdb } },
      WIDTH(ID3D11Texture3D1Vtbl) },
};
/* The bases every texture answers to, at the width of the narrowest texture. */
static const GUID k_iid_resource =
    { 0xdc8e63f3, 0xd12b, 0x4952, { 0xb4,0x7b,0x5e,0x45,0x02,0x6a,0x86,0x2d } };
static const GUID k_iid_device_child =
    { 0x1841e5c8, 0x16b0, 0x489b, { 0xbc,0xc8,0x44,0xcf,0xb0,0xd5,0xde,0xae } };

static HRESULT WINAPI tex_qi(void *self, const GUID *iid, void **out)
{
    tex_wrapper *w = self;
    const dxcts_iface *set;
    size_t count;
    D3D11_RESOURCE_DIMENSION dim;

    if (!iid || !out)
        return E_POINTER;
    ID3D11Resource_GetType((ID3D11Resource *)w->base.real, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
    {
        set = k_textures1d; count = sizeof(k_textures1d) / sizeof(k_textures1d[0]);
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        set = k_textures3d; count = sizeof(k_textures3d) / sizeof(k_textures3d[0]);
    }
    else
    {
        set = k_textures2d; count = sizeof(k_textures2d) / sizeof(k_textures2d[0]);
    }
    if (IsEqualGUID(iid, &k_iid_resource) || IsEqualGUID(iid, &k_iid_device_child)
        || dxcts_wrapper_answers(&w->base, iid, set, count))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    return ((dxcts_qi_fn)dxcts_real_vtable(self)[0])(w->base.real, iid, out);
}

/* GetDesc and GetDesc1 of every texture dimension share a slot index and put the
 * format at the same offset of their description. */
_Static_assert(SLOT(ID3D11Texture1DVtbl, GetDesc) == SLOT(ID3D11Texture2DVtbl, GetDesc)
               && SLOT(ID3D11Texture2DVtbl, GetDesc) == SLOT(ID3D11Texture3DVtbl, GetDesc)
               && SLOT(ID3D11Texture2D1Vtbl, GetDesc1) == SLOT(ID3D11Texture3D1Vtbl, GetDesc1),
               "texture GetDesc slots differ");

static void WINAPI tex_get_desc(void *self, void *desc)
{
    tex_wrapper *w = self;
    D3D11_RESOURCE_DIMENSION dim;
    size_t at;

    ((get_desc_fn)dxcts_real_vtable(self)[SLOT(ID3D11Texture2DVtbl, GetDesc)])(w->base.real,
                                                                               desc);
    if (!desc)
        return;
    ID3D11Resource_GetType((ID3D11Resource *)w->base.real, &dim);
    at = dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? offsetof(D3D11_TEXTURE1D_DESC, Format)
       : dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? offsetof(D3D11_TEXTURE3D_DESC, Format)
       : offsetof(D3D11_TEXTURE2D_DESC, Format);
    memcpy((unsigned char *)desc + at, &w->orig, sizeof(w->orig));
}

static void WINAPI tex_get_desc1(void *self, void *desc)
{
    tex_wrapper *w = self;
    D3D11_RESOURCE_DIMENSION dim;
    size_t at;

    ((get_desc_fn)dxcts_real_vtable(self)[SLOT(ID3D11Texture2D1Vtbl, GetDesc1)])(w->base.real,
                                                                                 desc);
    if (!desc)
        return;
    ID3D11Resource_GetType((ID3D11Resource *)w->base.real, &dim);
    at = dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? offsetof(D3D11_TEXTURE3D_DESC1, Format)
       : offsetof(D3D11_TEXTURE2D_DESC1, Format);
    memcpy((unsigned char *)desc + at, &w->orig, sizeof(w->orig));
}

/* Takes over the caller's reference on `tex`; returns `tex` itself if no wrapper could
 * be built, in which case GetDesc reports the twin. */
static void *wrap_texture(void *tex, DXGI_FORMAT orig)
{
    static int probed_1d, probed_2d, probed_3d;
    D3D11_RESOURCE_DIMENSION dim;
    tex_wrapper *w;
    int slots;

    ID3D11Resource_GetType((ID3D11Resource *)tex, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        slots = dxcts_widest_slots(tex, k_textures1d, 1, &probed_1d);
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        slots = dxcts_widest_slots(tex, k_textures3d, 2, &probed_3d);
    else
        slots = dxcts_widest_slots(tex, k_textures2d, 2, &probed_2d);
    w = (tex_wrapper *)dxcts_wrap(tex, slots, sizeof(*w));
    if (!w)
        return tex;
    w->orig = orig;
    w->base.vtable[0] = (void *)tex_qi;
    w->base.vtable[1] = (void *)dxcts_wrapper_addref;
    w->base.vtable[2] = (void *)dxcts_wrapper_release;
    w->base.vtable[SLOT(ID3D11Texture2DVtbl, GetDesc)] = (void *)tex_get_desc;
    if (slots > SLOT(ID3D11Texture2D1Vtbl, GetDesc1))
        w->base.vtable[SLOT(ID3D11Texture2D1Vtbl, GetDesc1)] = (void *)tex_get_desc1;
    return w;
}

/* Maps in flight whose bytes the layer shadows or must copy out at Unmap. */
typedef struct map_entry
{
    struct map_entry *next;
    void *res;
    UINT sub;
    D3D11_MAPPED_SUBRESOURCE real;
    D3D11_MAP type;
    /* rgb32: the API's 12-byte-texel view of the mapping; NULL for a depth-stencil
     * staging map, which reads the real mapping at Unmap. */
    unsigned char *shadow;
    UINT w, rows, slices, alpha;
} map_entry;

static map_entry *g_maps;

static void map_add(map_entry *e)
{
    lock();
    e->next = g_maps;
    g_maps = e;
    unlock();
}

static map_entry *map_take(void *res, UINT sub)
{
    map_entry **p, *e = NULL;

    lock();
    for (p = &g_maps; *p; p = &(*p)->next)
        if ((*p)->res == res && (*p)->sub == sub)
        {
            e = *p;
            *p = e->next;
            break;
        }
    unlock();
    return e;
}

/* --- depth-stencil uploads --- */
/*
 * D3DMetal writes only the depth plane of a D24_UNORM_S8_UINT or D32_FLOAT_S8X24_UINT
 * texture from CPU bytes: initial data, UpdateSubresource and a staging map all leave
 * the stencil at 0 (a staging map drops the stencil bytes at Unmap, and a D24S8
 * staging map drops the depth too), while a stencil the GPU wrote reads back through
 * a staging copy intact.  So every CPU write into a depth-stencil texture is followed
 * by a rewrite of the stencil plane from the same bytes: one point-list draw per
 * distinct stencil value, stencil op REPLACE with that value as the reference and
 * depth writes off, over a depth-stencil view of the written mip and slice.  Two more
 * quirks of the depth half: D3DMetal reads the source of a D32S8 upload as tightly
 * packed 4-byte depth texels, not the API's 8-byte ones, so those rows are repacked to
 * depth dwords first; and it drops D24S8 initial data outright, so a depth-stencil
 * texture is created without its initial data and filled through UpdateSubresource.
 * A staging depth-stencil texture keeps the bytes an Unmap wrote as private data, and
 * a copy out of it into a default texture is served as an UpdateSubresource of those
 * bytes.
 */
typedef struct
{
    tex_shape shape;
    UINT texel, stencil_at;
} ds_texture;

/* A 1D or 2D single-sampled texture of one of the two formats. */
static int ds_texture_of(ID3D11Resource *r, ds_texture *t)
{
    if (!tex_shape_of(r, &t->shape) || t->shape.dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D
        || t->shape.samples > 1)
        return 0;
    return dm_ds_layout(t->shape.format, &t->texel, &t->stencil_at);
}

static DXGI_FORMAT ds_view_format(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
               ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
}

static size_t ds_sub_bytes(const ds_texture *t, UINT sub, size_t *row_pitch)
{
    UINT mip = sub % t->shape.mips;

    *row_pitch = (size_t)mip_dim(t->shape.width, mip) * t->texel;
    return *row_pitch * mip_dim(t->shape.height, mip);
}

static size_t ds_sub_offset(const ds_texture *t, UINT sub)
{
    size_t off = 0, rp;
    UINT i;

    for (i = 0; i < sub; i++)
        off += ds_sub_bytes(t, i, &rp);
    return off;
}

/* The shadow of every subresource, zero-filled where none was recorded. */
static unsigned char *ds_shadow_get(ID3D11Resource *res, const ds_texture *t, size_t *total,
                                    int *had)
{
    unsigned char *out;
    UINT size;

    *total = ds_sub_offset(t, t->shape.mips * t->shape.slices);
    out = calloc(1, *total ? *total : 1);
    if (!out)
        return NULL;
    size = (UINT)*total;
    *had = SUCCEEDED(ID3D11Resource_GetPrivateData(res, &k_ds_shadow_tag, &size, out))
           && size == *total;
    return out;
}

/* The stencil draw's pipeline objects, made once per device through d3dcompiler_47. */
typedef HRESULT (WINAPI *compile_fn)(const void *, SIZE_T, const char *, const void *,
                                     void *, const char *, const char *, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);

static int stencil_pipe(dm_device *dw)
{
    ID3D11Device *dev = dxcts_real(dw);
    static const char k_vs[] =
        "float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }";
    static const D3D11_INPUT_ELEMENT_DESC k_elem =
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    D3D11_DEPTH_STENCIL_DESC dsd;
    D3D11_RASTERIZER_DESC rsd;
    ID3DBlob *code = NULL, *err = NULL;
    HMODULE compiler;
    compile_fn compile;
    HRESULT hr;

    if (dw->stencil_tried)
        return dw->st_vs != NULL;
    dw->stencil_tried = 1;
    compiler = LoadLibraryA("d3dcompiler_47.dll");
    compile = compiler ? (compile_fn)(void *)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile || FAILED(compile(k_vs, sizeof(k_vs) - 1, "stencil_vs", NULL, NULL,
                                   "main", "vs_4_0", 0, 0, &code, &err)) || !code)
    {
        release(err);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &dw->st_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(dev, &k_elem, 1,
                                            ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), &dw->st_il);
    release(code);
    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsd.StencilEnable = TRUE;
    dsd.StencilReadMask = 0xff;
    dsd.StencilWriteMask = 0xff;
    dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dsd.BackFace = dsd.FrontFace;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateDepthStencilState(dev, &dsd, &dw->st_dss);
    memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthClipEnable = FALSE;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRasterizerState(dev, &rsd, &dw->st_rs);
    if (FAILED(hr) || !dw->st_vs || !dw->st_il || !dw->st_dss || !dw->st_rs)
    {
        release(dw->st_vs); release(dw->st_il); release(dw->st_dss); release(dw->st_rs);
        dw->st_vs = NULL; dw->st_il = NULL; dw->st_dss = NULL; dw->st_rs = NULL;
        return 0;
    }
    return 1;
}

/* The pipeline state the stencil draws touch, saved around them. */
typedef struct
{
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11GeometryShader *gs;
    ID3D11HullShader *hs; ID3D11DomainShader *ds;
    ID3D11InputLayout *il; D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *vb; UINT vb_stride, vb_offset;
    ID3D11RasterizerState *rs; UINT n_vp, n_sc;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilState *dss; UINT stencil_ref;
    ID3D11BlendState *bs; FLOAT factor[4]; UINT sample_mask;
    ID3D11Buffer *so[D3D11_SO_BUFFER_SLOT_COUNT];
} saved_state;

static void save_state(ID3D11DeviceContext *c, saved_state *s)
{
    memset(s, 0, sizeof(*s));
    ID3D11DeviceContext_VSGetShader(c, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s->ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s->gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &s->hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &s->ds, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &s->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s->topo);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &s->vb, &s->vb_stride, &s->vb_offset);
    ID3D11DeviceContext_RSGetState(c, &s->rs);
    s->n_vp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(c, &s->n_vp, s->vp);
    s->n_sc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(c, &s->n_sc, s->sc);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                           s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s->dss, &s->stencil_ref);
    ID3D11DeviceContext_OMGetBlendState(c, &s->bs, s->factor, &s->sample_mask);
    ID3D11DeviceContext_SOGetTargets(c, D3D11_SO_BUFFER_SLOT_COUNT, s->so);
}

static void restore_state(ID3D11DeviceContext *c, saved_state *s)
{
    static const UINT so_off[D3D11_SO_BUFFER_SLOT_COUNT];
    size_t i;

    ID3D11DeviceContext_VSSetShader(c, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, s->ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, s->gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, s->hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, s->ds, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, s->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s->topo);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &s->vb, &s->vb_stride, &s->vb_offset);
    ID3D11DeviceContext_RSSetState(c, s->rs);
    ID3D11DeviceContext_RSSetViewports(c, s->n_vp, s->vp);
    ID3D11DeviceContext_RSSetScissorRects(c, s->n_sc, s->sc);
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                           s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s->dss, s->stencil_ref);
    ID3D11DeviceContext_OMSetBlendState(c, s->bs, s->factor, s->sample_mask);
    ID3D11DeviceContext_SOSetTargets(c, D3D11_SO_BUFFER_SLOT_COUNT, s->so, so_off);
    release(s->vs); release(s->ps); release(s->gs); release(s->hs); release(s->ds);
    release(s->il); release(s->vb); release(s->rs);
    for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        release(s->rtv[i]);
    release(s->dsv); release(s->dss); release(s->bs);
    for (i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT; i++)
        release(s->so[i]);
}

/* Write the stencil plane of subresource `sub` of `res` inside [x0, x1) x [y0, y1)
 * from `bytes` (texel-strided rows of `row_pitch`), on the real context. */
static void stencil_upload(dm_device *dw, ID3D11DeviceContext *ctx, ID3D11Resource *res,
                           const ds_texture *t, UINT sub, UINT x0, UINT y0, UINT x1,
                           UINT y1, const unsigned char *bytes, size_t row_pitch)
{
    ID3D11Device *dev = dxcts_real(dw);
    UINT mip = sub % t->shape.mips, slice = sub / t->shape.mips;
    UINT mw, mh, x, y, v, total = 0, first[256], count[256], cursor[256];
    float *all;
    ID3D11Buffer *vb = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd;
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView *no_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { 0 };
    ID3D11Buffer *no_so[D3D11_SO_BUFFER_SLOT_COUNT] = { 0 };
    static const UINT so_off[D3D11_SO_BUFFER_SLOT_COUNT];
    const UINT stride = 8, offset = 0;
    saved_state saved;

    if (!(t->shape.bind & D3D11_BIND_DEPTH_STENCIL) || t->shape.usage != D3D11_USAGE_DEFAULT
        || slice >= t->shape.slices)
        return;
    mw = mip_dim(t->shape.width, mip);
    mh = mip_dim(t->shape.height, mip);
    if (x1 > mw) x1 = mw;
    if (y1 > mh) y1 = mh;
    if (x0 >= x1 || y0 >= y1)
        return;
    /* Points grouped by stencil value: a run per value into one buffer. */
    memset(count, 0, sizeof(count));
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            count[bytes[(size_t)(y - y0) * row_pitch + (size_t)(x - x0) * t->texel
                        + t->stencil_at]]++;
    count[0] = 0;   /* the depth write already left 0 there */
    for (v = 0; v < 256; v++)
    {
        first[v] = total;
        cursor[v] = total;
        total += count[v];
    }
    if (!total)
        return;
    all = malloc((size_t)total * 2 * sizeof(float));
    if (!all)
        return;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
        {
            v = bytes[(size_t)(y - y0) * row_pitch + (size_t)(x - x0) * t->texel
                      + t->stencil_at];
            if (!v)
                continue;
            all[2 * cursor[v]] = ((float)x + 0.5f) / (float)mw * 2.f - 1.f;
            all[2 * cursor[v] + 1] = 1.f - ((float)y + 0.5f) / (float)mh * 2.f;
            cursor[v]++;
        }
    if (stencil_pipe(dw))
    {
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = total * 2 * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init.pSysMem = all; init.SysMemPitch = 0; init.SysMemSlicePitch = 0;
        ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);
        memset(&vd, 0, sizeof(vd));
        vd.Format = ds_view_format(t->shape.format);
        if (t->shape.dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray.MipSlice = mip;
            vd.Texture1DArray.FirstArraySlice = slice;
            vd.Texture1DArray.ArraySize = 1;
        }
        else
        {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = mip;
            vd.Texture2DArray.FirstArraySlice = slice;
            vd.Texture2DArray.ArraySize = 1;
        }
        ID3D11Device_CreateDepthStencilView(dev, res, &vd, &dsv);
    }
    free(all);
    if (!vb || !dsv)
    {
        release(vb); release(dsv);
        return;
    }
    save_state(ctx, &saved);
    ID3D11DeviceContext_SOSetTargets(ctx, D3D11_SO_BUFFER_SLOT_COUNT, no_so, so_off);
    ID3D11DeviceContext_VSSetShader(ctx, dw->st_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, dw->st_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_RSSetState(ctx, dw->st_rs);
    vp.TopLeftX = 0.f; vp.TopLeftY = 0.f; vp.Width = (float)mw; vp.Height = (float)mh;
    vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                           no_rtv, dsv);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffffu);
    for (v = 1; v < 256; v++)
    {
        if (!count[v])
            continue;
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dw->st_dss, v);
        ID3D11DeviceContext_Draw(ctx, count[v], first[v]);
    }
    restore_state(ctx, &saved);
    release(vb);
    release(dsv);
}

/* UpdateSubresource of API-layout depth-stencil bytes on the real context: the depth
 * plane through D3DMetal (D32S8 rows repacked to the 4-byte texels it reads), the
 * stencil plane through the draws. */
static void ds_update(dm_device *dw, ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                      const ds_texture *t, UINT sub, const D3D11_BOX *box,
                      const unsigned char *src, UINT src_row)
{
    UINT mip = sub % t->shape.mips;
    UINT x0 = box ? box->left : 0, y0 = box ? box->top : 0;
    UINT x1 = box ? box->right : mip_dim(t->shape.width, mip);
    UINT y1 = box ? box->bottom : mip_dim(t->shape.height, mip);
    size_t rp;

    if (x0 >= x1 || y0 >= y1)
        return;
    rp = src_row ? src_row : (size_t)(x1 - x0) * t->texel;
    if (t->texel == 8)
    {
        UINT w = x1 - x0, rows = y1 - y0, x, y;
        unsigned char *depth = malloc((size_t)w * 4 * rows);

        if (!depth)
            return;
        for (y = 0; y < rows; y++)
            for (x = 0; x < w; x++)
                memcpy(depth + ((size_t)y * w + x) * 4, src + (size_t)y * rp + (size_t)x * 8, 4);
        ID3D11DeviceContext_UpdateSubresource(ctx, dst, sub, box, depth, w * 4, 0);
        free(depth);
    }
    else
        ID3D11DeviceContext_UpdateSubresource(ctx, dst, sub, box, src, (UINT)rp, 0);
    stencil_upload(dw, ctx, dst, t, sub, x0, y0, x1, y1, src, rp);
}

/* --- block-compressed to uncompressed copies --- */
/*
 * D3D copies between a block-compressed texture and an uncompressed one of the block's
 * size (a BC1 block to an R16G16B16A16 texel, a BC7 block to an R32G32B32A32 one) as
 * raw bits.  D3DMetal encodes the compressed source with a Metal blit that dies inside
 * the GPU driver (SIGSEGV in AGX copyTextureToBuffer), taking the process - or the
 * render-server worker and the guest device - with it.  The other direction, and a
 * copy to a staging twin of the source, work.  So the source region is read back
 * through a staging twin and its bytes are written into the destination with
 * UpdateSubresource: one round trip, only on this copy shape.
 */
static UINT bc_block_bytes(DXGI_FORMAT f)
{
    if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB)
        || (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM))
        return 8;
    if ((f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB)
        || (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM)
        || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB))
        return 16;
    return 0;
}

/* A default-usage texture shaped like staging texture `res`, or NULL when `res` is not
 * a staging texture (or the twin cannot be made). */
static ID3D11Resource *default_twin(ID3D11Device *dev, ID3D11Resource *res)
{
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource *twin = NULL;

    ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC d;

        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)res, &d);
        if (d.Usage != D3D11_USAGE_STAGING)
            return NULL;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture3D(dev, &d, NULL, (ID3D11Texture3D **)&twin)))
            return NULL;
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC d;

        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
        if (d.Usage != D3D11_USAGE_STAGING)
            return NULL;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, (ID3D11Texture2D **)&twin)))
            return NULL;
    }
    return twin;
}

/* 1 when the copy was served here.  Handles the 2D and 3D shapes. */
static int copy_bc_to_uncompressed(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                   ID3D11Resource *dst, UINT dst_sub, UINT x, UINT y,
                                   UINT z, ID3D11Resource *src, UINT src_sub,
                                   const D3D11_BOX *box)
{
    tex_shape s, d;
    UINT block, mip, mw, mh, md, left = 0, top = 0, front = 0, right, bottom, back;
    UINT bx, by, bw, bh, nd, r, sl;
    ID3D11Resource *staging = NULL, *target;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX dbox;
    unsigned char *rows;
    size_t slice_bytes;
    HRESULT hr;
    int is3d;

    if (!tex_shape_of(src, &s) || !tex_shape_of(dst, &d) || s.dim != d.dim)
        return 0;
    is3d = s.dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    if (s.dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D && !is3d)
        return 0;
    if (!is3d && z)
        return 0;
    block = bc_block_bytes(s.format);
    if (!block || bc_block_bytes(d.format) || s.samples > 1
        || src_sub >= s.mips * s.slices)
        return 0;
    mip = is3d ? src_sub : src_sub % s.mips;
    mw = mip_dim(s.width, mip);
    mh = mip_dim(s.height, mip);
    md = is3d ? mip_dim(s.depth, mip) : 1;
    right = mw; bottom = mh; back = md;
    if (box)
    {
        if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)
            return 0;
        if (!is3d && (box->front || box->back != 1))
            return 0;
        left = box->left; top = box->top; front = box->front;
        right = box->right; bottom = box->bottom; back = box->back;
    }
    if ((left | top) & 3 || right > mw || bottom > mh || back > md)
        return 0;
    bx = left / 4; by = top / 4;
    bw = (right - left + 3) / 4; bh = (bottom - top + 3) / 4;
    nd = back - front;
    if (is3d)
    {
        D3D11_TEXTURE3D_DESC sd;

        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)src, &sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture3D(dev, &sd, NULL, (ID3D11Texture3D **)&staging);
    }
    else
    {
        D3D11_TEXTURE2D_DESC sd;

        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        hr = ID3D11Device_CreateTexture2D(dev, &sd, NULL, (ID3D11Texture2D **)&staging);
    }
    if (FAILED(hr) || !staging)
        return 0;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, staging, src_sub, 0, 0, 0, src,
                                              src_sub, NULL);
    if (FAILED(ID3D11DeviceContext_Map(ctx, staging, src_sub, D3D11_MAP_READ, 0, &m)))
    {
        release(staging);
        return 0;
    }
    slice_bytes = (size_t)bw * bh * block;
    rows = malloc(slice_bytes * nd);
    if (rows)
    {
        for (sl = 0; sl < nd; sl++)
            for (r = 0; r < bh; r++)
                memcpy(rows + sl * slice_bytes + (size_t)r * bw * block,
                       (const unsigned char *)m.pData + (size_t)(front + sl) * m.DepthPitch
                           + (size_t)(by + r) * m.RowPitch + (size_t)bx * block,
                       (size_t)bw * block);
    }
    ID3D11DeviceContext_Unmap(ctx, staging, src_sub);
    release(staging);
    if (!rows)
        return 0;
    dbox.left = x; dbox.top = y; dbox.front = z;
    dbox.right = x + bw; dbox.bottom = y + bh; dbox.back = z + nd;
    /* UpdateSubresource cannot write a staging texture (the runtime drops the call),
     * so a staging destination is written through a default-usage twin. */
    if (!(target = default_twin(dev, dst)))
        target = dst;
    ID3D11DeviceContext_UpdateSubresource(ctx, target, dst_sub, &dbox, rows, bw * block,
                                          is3d ? (UINT)slice_bytes : 0);
    if (target != dst)
    {
        ID3D11DeviceContext_CopySubresourceRegion(ctx, dst, dst_sub, x, y, z, target,
                                                  dst_sub, &dbox);
        release(target);
    }
    free(rows);
    return 1;
}

/* CopyResource between a block-compressed texture and its uncompressed twin (block
 * dimensions equal the twin's texel dimensions): every subresource through the path
 * above.  1 when served here. */
static int copy_resource_bc_to_uncompressed(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                            ID3D11Resource *dst, ID3D11Resource *src)
{
    tex_shape s, d;
    UINT sub, n;

    if (!tex_shape_of(src, &s) || !tex_shape_of(dst, &d) || s.dim != d.dim
        || (s.dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D
            && s.dim != D3D11_RESOURCE_DIMENSION_TEXTURE3D))
        return 0;
    if (!bc_block_bytes(s.format) || bc_block_bytes(d.format) || s.mips != d.mips
        || s.slices != d.slices)
        return 0;
    n = s.mips * s.slices;
    for (sub = 0; sub < n; sub++)
        if (!copy_bc_to_uncompressed(dev, ctx, dst, sub, 0, 0, 0, src, sub, NULL))
            return 0;
    return 1;
}

/* D3D11 allows no destination box on a depth-stencil resource: the runtime drops an
 * UpdateSubresource that names one, even a box covering the whole subresource.  A box
 * that covers it is therefore passed as NULL. */
static const D3D11_BOX *ds_box_or_null(const ds_texture *t, UINT sub, const D3D11_BOX *box)
{
    UINT mip = sub % t->shape.mips;

    if (box->left == 0 && box->top == 0 && box->right == mip_dim(t->shape.width, mip)
        && box->bottom == mip_dim(t->shape.height, mip))
        return NULL;
    return box;
}

/* A copy out of a staging depth-stencil texture whose bytes are shadowed is served as
 * an UpdateSubresource of those bytes (which rewrites the stencil); a copy INTO such a
 * staging texture invalidates its shadow.  1 when the copy was served here. */
static int copy_ds_shadowed(dm_device *dw, ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                            UINT dst_sub, UINT x, UINT y, ID3D11Resource *src, UINT src_sub,
                            const D3D11_BOX *box, int whole)
{
    ds_texture st, dt;
    unsigned char *shadow;
    size_t total;
    int had;
    UINT i, n;

    if (!ds_texture_of(src, &st) || !ds_texture_of(dst, &dt)
        || st.shape.format != dt.shape.format)
        return 0;
    if (dt.shape.usage == D3D11_USAGE_STAGING)
    {
        ID3D11Resource_SetPrivateData(dst, &k_ds_shadow_tag, 0, NULL);
        return 0;
    }
    if (st.shape.usage != D3D11_USAGE_STAGING || dt.shape.usage != D3D11_USAGE_DEFAULT)
        return 0;
    shadow = ds_shadow_get(src, &st, &total, &had);
    if (!shadow)
        return 0;
    if (!had)
    {
        free(shadow);
        return 0;
    }
    n = whole ? st.shape.mips * st.shape.slices : 1;
    for (i = 0; i < n; i++)
    {
        UINT ss = whole ? i : src_sub, ds = whole ? i : dst_sub;
        size_t rp, bytes;
        UINT sw, sh, l = 0, t = 0, r, b;
        D3D11_BOX dbox;

        if (ss >= st.shape.mips * st.shape.slices || ds >= dt.shape.mips * dt.shape.slices)
            continue;
        bytes = ds_sub_bytes(&st, ss, &rp);
        sw = (UINT)(rp / st.texel);
        sh = (UINT)(bytes / rp);
        r = sw; b = sh;
        if (!whole && box)
        {
            l = box->left; t = box->top; r = box->right; b = box->bottom;
        }
        if (r > sw) r = sw;
        if (b > sh) b = sh;
        if (l >= r || t >= b)
            continue;
        dbox.left = whole ? 0 : x; dbox.top = whole ? 0 : y; dbox.front = 0;
        dbox.right = dbox.left + (r - l); dbox.bottom = dbox.top + (b - t); dbox.back = 1;
        ds_update(dw, ctx, dst, &dt, ds, ds_box_or_null(&dt, ds, &dbox),
                  shadow + ds_sub_offset(&st, ss) + (size_t)t * rp + (size_t)l * st.texel,
                  (UINT)rp);
    }
    free(shadow);
    return 1;
}

/* A CopySubresourceRegion within one subresource whose source and destination boxes
 * overlap in depth is undefined by D3D, but the HLK expects the result of a copy
 * through a temporary (every destination slice receives the source slice as it was
 * before the copy).  D3DMetal's blit writes the slices in order and reads the one it
 * just wrote, so the overlapping 3D case goes through a temporary texture here.  1 when
 * served. */
static int copy_overlapping_via_temp(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                     ID3D11Resource *dst, UINT dst_sub, UINT x, UINT y,
                                     UINT z, ID3D11Resource *src, UINT src_sub,
                                     const D3D11_BOX *box)
{
    tex_shape d;
    D3D11_TEXTURE3D_DESC td;
    D3D11_BOX whole;
    ID3D11Texture3D *tmp = NULL;
    UINT depth;
    int xy_overlap, z_overlap;

    if (dst != src || dst_sub != src_sub || !box || !tex_shape_of(dst, &d)
        || d.dim != D3D11_RESOURCE_DIMENSION_TEXTURE3D || dst_sub >= d.mips)
        return 0;
    if (box->back <= box->front + 1 || box->right <= box->left || box->bottom <= box->top)
        return 0;
    xy_overlap = x < box->right && x + (box->right - box->left) > box->left
              && y < box->bottom && y + (box->bottom - box->top) > box->top;
    depth = box->back - box->front;
    z_overlap = z < box->back && z + depth > box->front;
    if (!xy_overlap || !z_overlap || z == box->front)
        return 0;
    memset(&td, 0, sizeof(td));
    td.Width = box->right - box->left;
    td.Height = box->bottom - box->top;
    td.Depth = depth;
    td.MipLevels = 1;
    td.Format = d.format;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(ID3D11Device_CreateTexture3D(dev, &td, NULL, &tmp)) || !tmp)
        return 0;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmp, 0, 0, 0, 0, src,
                                              src_sub, box);
    whole.left = 0; whole.top = 0; whole.front = 0;
    whole.right = td.Width; whole.bottom = td.Height; whole.back = td.Depth;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, dst, dst_sub, x, y, z,
                                              (ID3D11Resource *)tmp, 0, &whole);
    release(tmp);
    return 1;
}

/* --- context hooks: copies and CPU byte paths --- */

static ID3D11Device *ctx_real_device(dm_context *w)
{
    return (ID3D11Device *)dxcts_real(w->device);
}

static void WINAPI ctx_copy_resource(void *self, ID3D11Resource *dst, ID3D11Resource *src)
{
    dm_context *w = self;
    ID3D11DeviceContext *real = dxcts_real(self);

    dst = unwrap(dst);
    src = unwrap(src);

    if (dm_knob_depth_stencil_upload()
        && copy_ds_shadowed(w->device, real, dst, 0, 0, 0, src, 0, NULL, 1))
        return;
    if (dm_knob_bc_copy() && copy_resource_bc_to_uncompressed(ctx_real_device(w), real, dst, src))
        return;
    ((copy_resource_fn)REAL_CTX(self, CopyResource))(real, dst, src);
}

static int copy_region_served(dm_context *w, ID3D11Resource *dst, UINT dst_sub, UINT x,
                              UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                              const D3D11_BOX *box)
{
    ID3D11DeviceContext *real = dxcts_real(w);
    ID3D11Device *dev = ctx_real_device(w);

    if (dm_knob_depth_stencil_upload()
        && copy_ds_shadowed(w->device, real, dst, dst_sub, x, y, src, src_sub, box, 0))
        return 1;
    if (dm_knob_bc_copy()
        && (copy_bc_to_uncompressed(dev, real, dst, dst_sub, x, y, z, src, src_sub, box)
            || copy_overlapping_via_temp(dev, real, dst, dst_sub, x, y, z, src, src_sub,
                                         box)))
        return 1;
    return 0;
}

static void WINAPI ctx_copy_region(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
                                   UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                                   const D3D11_BOX *box)
{
    dst = unwrap(dst);
    src = unwrap(src);
    if (copy_region_served(self, dst, dst_sub, x, y, z, src, src_sub, box))
        return;
    ((copy_region_fn)REAL_CTX(self, CopySubresourceRegion))(dxcts_real(self), dst, dst_sub,
                                                            x, y, z, src, src_sub, box);
}

static void WINAPI ctx_copy_region1(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
                                    UINT y, UINT z, ID3D11Resource *src, UINT src_sub,
                                    const D3D11_BOX *box, UINT flags)
{
    dst = unwrap(dst);
    src = unwrap(src);
    if (copy_region_served(self, dst, dst_sub, x, y, z, src, src_sub, box))
        return;
    ((copy_region1_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl,
                                                   CopySubresourceRegion1)])(
        dxcts_real(self), dst, dst_sub, x, y, z, src, src_sub, box, flags);
}

/* 1 when the update was served here. */
static int update_served(dm_context *w, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                         const void *src, UINT src_row, UINT src_depth, UINT flags,
                         int has_flags)
{
    ID3D11DeviceContext *real = dxcts_real(w);
    ds_texture dt;
    rgb32_tag t;
    UINT wd, rows, slices;
    size_t srow, sslice, drow, dslice;
    unsigned char *packed;

    if (!src)
        return 0;
    if (dm_knob_depth_stencil_upload() && ds_texture_of(dst, &dt)
        && sub < dt.shape.mips * dt.shape.slices)
    {
        /* The copy-flag hints do not survive the depth-stencil path. */
        ds_update(w->device, real, dst, &dt, sub, box, src, src_row);
        return 1;
    }
    if (!dm_knob_rgb32() || !rgb32_tagged(dst, &t) || !rgb32_extent(dst, sub, &wd, &rows, &slices))
        return 0;
    if (box)
    {
        wd = box->right - box->left; rows = box->bottom - box->top;
        slices = box->back - box->front;
    }
    srow = src_row ? src_row : (size_t)wd * 12;
    sslice = src_depth ? src_depth : srow * rows;
    drow = (size_t)wd * 16;
    dslice = drow * rows;
    packed = malloc(dslice * slices);
    if (!packed)
        return 0;
    rgb32_pack(packed, drow, dslice, src, srow, sslice, wd, rows, slices, t.alpha_bits);
    if (has_flags)
        ((update1_fn)dxcts_real_vtable(w)[SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1)])(
            real, dst, sub, box, packed, (UINT)drow, (UINT)dslice, flags);
    else
        ID3D11DeviceContext_UpdateSubresource(real, dst, sub, box, packed, (UINT)drow,
                                              (UINT)dslice);
    free(packed);
    return 1;
}

static void WINAPI ctx_update(void *self, ID3D11Resource *dst, UINT sub,
                              const D3D11_BOX *box, const void *src, UINT src_row,
                              UINT src_depth)
{
    dst = unwrap(dst);
    if (update_served(self, dst, sub, box, src, src_row, src_depth, 0, 0))
        return;
    ((update_fn)REAL_CTX(self, UpdateSubresource))(dxcts_real(self), dst, sub, box, src,
                                                   src_row, src_depth);
}

static void WINAPI ctx_update1(void *self, ID3D11Resource *dst, UINT sub,
                               const D3D11_BOX *box, const void *src, UINT src_row,
                               UINT src_depth, UINT flags)
{
    dst = unwrap(dst);
    if (update_served(self, dst, sub, box, src, src_row, src_depth, flags, 1))
        return;
    ((update1_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1)])(
        dxcts_real(self), dst, sub, box, src, src_row, src_depth, flags);
}

static HRESULT WINAPI ctx_map(void *self, ID3D11Resource *res, UINT sub, D3D11_MAP type,
                              UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    map_fn real_map = (map_fn)REAL_CTX(self, Map);
    ID3D11DeviceContext *real = dxcts_real(self);
    ds_texture dt;
    rgb32_tag t;
    UINT w, rows, slices;
    map_entry *e;
    HRESULT hr;

    res = unwrap(res);
    if (mapped && dm_knob_depth_stencil_upload() && type != D3D11_MAP_READ
        && ds_texture_of(res, &dt) && dt.shape.usage == D3D11_USAGE_STAGING)
    {
        hr = real_map(real, res, sub, type, flags, mapped);
        if (SUCCEEDED(hr) && mapped->pData && (e = calloc(1, sizeof(*e))))
        {
            e->res = res; e->sub = sub; e->real = *mapped; e->type = type;
            map_add(e);
        }
        return hr;
    }
    if (!mapped || !dm_knob_rgb32() || !rgb32_tagged(res, &t)
        || !rgb32_extent(res, sub, &w, &rows, &slices))
        return real_map(real, res, sub, type, flags, mapped);
    e = calloc(1, sizeof(*e));
    if (!e)
        return real_map(real, res, sub, type, flags, mapped);
    hr = real_map(real, res, sub, type, flags, &e->real);
    if (FAILED(hr) || !e->real.pData)
    {
        free(e);
        return hr;
    }
    e->res = res; e->sub = sub; e->type = type; e->w = w; e->rows = rows;
    e->slices = slices; e->alpha = t.alpha_bits;
    e->shadow = calloc(1, (size_t)w * 12 * rows * slices);
    if (!e->shadow)
    {
        ID3D11DeviceContext_Unmap(real, res, sub);
        free(e);
        return E_OUTOFMEMORY;
    }
    if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE || type == D3D11_MAP_WRITE)
        rgb32_unpack(e->shadow, (size_t)w * 12, (size_t)w * 12 * rows, e->real.pData,
                     e->real.RowPitch,
                     e->real.DepthPitch ? e->real.DepthPitch : (size_t)e->real.RowPitch * rows,
                     w, rows, slices);
    mapped->pData = e->shadow;
    mapped->RowPitch = w * 12;
    mapped->DepthPitch = w * 12 * rows;
    map_add(e);
    return hr;
}

static void WINAPI ctx_unmap(void *self, ID3D11Resource *res, UINT sub)
{
    ID3D11DeviceContext *real = dxcts_real(self);
    map_entry *e;

    res = unwrap(res);
    e = map_take(res, sub);

    if (e && !e->shadow)
    {
        ds_texture dt;

        if (ds_texture_of(res, &dt) && sub < dt.shape.mips * dt.shape.slices)
        {
            size_t total, rp, bytes;
            int had;
            unsigned char *shadow = ds_shadow_get(res, &dt, &total, &had);

            if (shadow)
            {
                UINT rows, y;
                unsigned char *dst;

                bytes = ds_sub_bytes(&dt, sub, &rp);
                rows = (UINT)(bytes / rp);
                dst = shadow + ds_sub_offset(&dt, sub);
                for (y = 0; y < rows; y++)
                    memcpy(dst + y * rp,
                           (const unsigned char *)e->real.pData + (size_t)y * e->real.RowPitch,
                           rp);
                ID3D11Resource_SetPrivateData(res, &k_ds_shadow_tag, (UINT)total, shadow);
                free(shadow);
            }
        }
    }
    else if (e && e->type != D3D11_MAP_READ)
        rgb32_pack(e->real.pData, e->real.RowPitch,
                   e->real.DepthPitch ? e->real.DepthPitch : (size_t)e->real.RowPitch * e->rows,
                   e->shadow, (size_t)e->w * 12, (size_t)e->w * 12 * e->rows, e->w, e->rows,
                   e->slices, e->alpha);
    if (e)
    {
        free(e->shadow);
        free(e);
    }
    ((unmap_fn)REAL_CTX(self, Unmap))(real, res, sub);
}

static void WINAPI ctx_resolve(void *self, ID3D11Resource *dst, UINT dst_sub,
                               ID3D11Resource *src, UINT src_sub, DXGI_FORMAT format)
{
    DXGI_FORMAT twin;
    UINT alpha;

    dst = unwrap(dst);
    src = unwrap(src);
    if (dm_knob_rgb32() && rgb32_twin(format, &twin, &alpha))
        format = twin;
    ((resolve_fn)REAL_CTX(self, ResolveSubresource))(dxcts_real(self), dst, dst_sub, src,
                                                     src_sub, format);
}

/* A 3-channel target has no alpha to clear: the twin's alpha stays 1, the value a
 * 3-channel format reads. */
static void WINAPI ctx_clear_rtv(void *self, ID3D11RenderTargetView *rtv, const FLOAT color[4])
{
    clear_rtv_fn real = (clear_rtv_fn)REAL_CTX(self, ClearRenderTargetView);

    if (color && dm_knob_rgb32() && rgb32_view((ID3D11View *)rtv))
    {
        FLOAT c[4] = { color[0], color[1], color[2], 1.f };

        real(dxcts_real(self), rtv, c);
        return;
    }
    real(dxcts_real(self), rtv, color);
}


/* The remaining entry points that take a resource: unwrapped and forwarded. */
typedef void (WINAPI *discard_fn)(void *, ID3D11Resource *);
typedef void (WINAPI *set_minlod_fn)(void *, ID3D11Resource *, FLOAT);
typedef FLOAT (WINAPI *get_minlod_fn)(void *, ID3D11Resource *);
typedef HRESULT (WINAPI *create_dsv_fn)(void *, ID3D11Resource *,
                                       const D3D11_DEPTH_STENCIL_VIEW_DESC *, void **);

static void WINAPI ctx_discard_resource(void *self, ID3D11Resource *res)
{
    ((discard_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl, DiscardResource)])(
        dxcts_real(self), unwrap(res));
}

static void WINAPI ctx_set_min_lod(void *self, ID3D11Resource *res, FLOAT lod)
{
    ((set_minlod_fn)REAL_CTX(self, SetResourceMinLOD))(dxcts_real(self), unwrap(res), lod);
}

static FLOAT WINAPI ctx_get_min_lod(void *self, ID3D11Resource *res)
{
    return ((get_minlod_fn)REAL_CTX(self, GetResourceMinLOD))(dxcts_real(self), unwrap(res));
}

static HRESULT WINAPI dev_create_dsv(void *self, ID3D11Resource *res,
                                     const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, void **out)
{
    return ((create_dsv_fn)REAL_DEV(self, CreateDepthStencilView))(dxcts_real(self),
                                                                   unwrap(res), desc, out);
}

/* --- DXBC readers --- */

typedef struct
{
    const unsigned char *data;
    UINT size;
} dxbc_chunk;

static int dxbc_find(const void *code, size_t len, const char *tag, dxbc_chunk *out)
{
    const unsigned char *p = code;
    UINT count, i;

    if (!p || len < 32 || memcmp(p, "DXBC", 4) != 0)
        return 0;
    memcpy(&count, p + 28, 4);
    if (count > 64 || 32 + 4u * count > len)
        return 0;
    for (i = 0; i < count; i++)
    {
        UINT off, size;

        memcpy(&off, p + 32 + 4 * i, 4);
        if (off > len || len - off < 8)
            return 0;
        memcpy(&size, p + off + 4, 4);
        if (size > len - off - 8)
            return 0;
        if (memcmp(p + off, tag, 4) == 0)
        {
            out->data = p + off + 8;
            out->size = size;
            return 1;
        }
    }
    return 0;
}

typedef struct
{
    const char *name;
    UINT index, reg, mask;
} sig_entry;

/* ISGN/OSGN (24-byte entries), OSG5 (28, stream first) and ISG1/OSG1 (32, stream
 * first, precision last).  Entries on a stream other than 0 fail. */
static int signature_entries(const dxbc_chunk *c, UINT entry_size, sig_entry *out,
                             UINT *n_out)
{
    UINT count, first, lead, i;

    if (c->size < 8)
        return 0;
    memcpy(&count, c->data, 4);
    memcpy(&first, c->data + 4, 4);
    if (count > 64 || first > c->size || (c->size - first) / entry_size < count)
        return 0;
    lead = entry_size == 24 ? 0 : 4;
    for (i = 0; i < count; i++)
    {
        const unsigned char *e = c->data + first + i * entry_size;
        UINT stream = 0, name_off, index, reg;

        if (lead)
            memcpy(&stream, e, 4);
        memcpy(&name_off, e + lead, 4);
        memcpy(&index, e + lead + 4, 4);
        memcpy(&reg, e + lead + 16, 4);
        if (stream != 0 || name_off >= c->size || reg >= 32)
            return 0;
        if (!memchr(c->data + name_off, 0, c->size - name_off))
            return 0;
        out[i].name = (const char *)(c->data + name_off);
        out[i].index = index;
        out[i].reg = reg;
        out[i].mask = e[lead + 20];
    }
    *n_out = count;
    return 1;
}

static int output_signature(const void *code, size_t len, dxbc_chunk *c, UINT *entry_size)
{
    if (dxbc_find(code, len, "OSGN", c)) { *entry_size = 24; return 1; }
    if (dxbc_find(code, len, "OSG5", c)) { *entry_size = 28; return 1; }
    if (dxbc_find(code, len, "OSG1", c)) { *entry_size = 32; return 1; }
    return 0;
}

static int signatures_identical(const void *code, size_t len)
{
    dxbc_chunk in, out;
    UINT in_size = 24, out_size, na, nb, i;
    sig_entry a[64], b[64];

    if (!dxbc_find(code, len, "ISGN", &in))
    {
        if (!dxbc_find(code, len, "ISG1", &in))
            return 0;
        in_size = 32;
    }
    if (!output_signature(code, len, &out, &out_size))
        return 0;
    if (!signature_entries(&in, in_size, a, &na) || !signature_entries(&out, out_size, b, &nb))
        return 0;
    if (!na || na != nb)
        return 0;
    for (i = 0; i < na; i++)
        if (strcmp(a[i].name, b[i].name) != 0 || a[i].index != b[i].index
            || a[i].reg != b[i].reg || a[i].mask != b[i].mask)
            return 0;
    return 1;
}

/* --- pass-through geometry shaders --- */
/*
 * D3DMetal folds a geometry shader that emits its input vertices unchanged into the
 * stages around it, but the pipeline keeps an entry for it and, with a hull and domain
 * shader bound, still draws through the mesh pipeline - whose descriptor then has no
 * mesh function.  Metal validation aborts on that; without validation the draw divides
 * by an uninitialised thread count and the process dies of SIGFPE.  Such a shader is
 * tagged at creation and never bound: the picture is the same without it, which is
 * what D3DMetal makes of it outside tessellation.
 *
 * Detection reads the SM4/SM5 token stream: input primitive point or line - the two
 * D3DMetal folds - a matching output topology, and a body that is nothing but, for
 * each input vertex in order, a full copy of every declared output register from the
 * same register of that vertex followed by one emit, then ret.  The input and output
 * signatures must agree register for register.  Anything else - a swizzle, a
 * modifier, an arithmetic op, an extra emit, a constant, a DXIL body - leaves the
 * shader alone.
 */
enum
{
    OP_EMIT = 19, OP_MOV = 54, OP_RET = 62, OP_CUSTOM_DATA = 53,
    OP_DCL_OUTPUT_TOPOLOGY = 92, OP_DCL_INPUT_PRIMITIVE = 93,
    OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SGV = 102, OP_DCL_OUTPUT_SIV = 103,
    OP_EMIT_STREAM = 117,
    OPERAND_INPUT = 1, OPERAND_OUTPUT = 2,
};

static int is_declaration(UINT op)
{
    return (op >= 88 && op <= 106) || (op >= 143 && op <= 162);
}

/* A 4-component operand with no extension, immediate32 indices only. */
static int plain_operand(UINT tok, UINT type, UINT dims)
{
    return (tok & 3) == 2 && !(tok >> 31) && ((tok >> 12) & 0xff) == type
        && ((tok >> 20) & 3) == dims && ((tok >> 22) & 0x1ff) == 0;
}

static int body_is_pass_through(const dxbc_chunk *c)
{
    const UINT *t = (const UINT *)c->data;
    UINT n = c->size / 4, i;
    UINT vertices = 0, topology = 0, declared = 0, written = 0, emitted = 0;
    int done = 0;

    if (c->size < 8 || c->size % 4)
        return 0;
    if ((t[0] >> 16) != 2 || t[1] > n)  /* program type: geometry */
        return 0;
    for (i = 2; i < t[1] && !done;)
    {
        UINT tok = t[i], op = tok & 0x7ff, len = (tok >> 24) & 0x7f;

        if (op == OP_CUSTOM_DATA)
            len = i + 1 < n ? t[i + 1] : 0;
        if (len == 0 || len > t[1] - i)
            return 0;
        if (is_declaration(op))
        {
            if (op == OP_DCL_INPUT_PRIMITIVE)
            {
                UINT prim = (tok >> 11) & 0x3f;

                vertices = prim == 1 ? 1 : prim == 2 ? 2 : 0;
                if (!vertices)
                    return 0;
            }
            else if (op == OP_DCL_OUTPUT_TOPOLOGY)
                topology = (tok >> 11) & 0x7f;
            else if (op == OP_DCL_OUTPUT || op == OP_DCL_OUTPUT_SGV || op == OP_DCL_OUTPUT_SIV)
            {
                if (len < 3 || !plain_operand(t[i + 1], OPERAND_OUTPUT, 1) || t[i + 2] >= 32)
                    return 0;
                declared |= 1u << t[i + 2];
            }
        }
        else if (op == OP_MOV)
        {
            /* mov oN.mask, v[k][N].xyzw -- no saturate, nothing extended */
            UINT dst, reg, src, mask, swizzle, comp;

            if (len != 6 || (tok >> 11) & 0x1fff)
                return 0;
            dst = t[i + 1]; reg = t[i + 2]; src = t[i + 3];
            if (!plain_operand(dst, OPERAND_OUTPUT, 1) || ((dst >> 2) & 3) != 0 || reg >= 32
                || !(declared & (1u << reg)) || (written & (1u << reg)))
                return 0;
            if (!plain_operand(src, OPERAND_INPUT, 2) || ((src >> 2) & 3) != 1
                || t[i + 4] != emitted || t[i + 5] != reg)
                return 0;
            mask = (dst >> 4) & 0xf;
            swizzle = (src >> 4) & 0xff;
            for (comp = 0; comp < 4; comp++)
                if ((mask & (1u << comp)) && ((swizzle >> (2 * comp)) & 3) != comp)
                    return 0;
            written |= 1u << reg;
        }
        else if (op == OP_EMIT || op == OP_EMIT_STREAM)
        {
            if (op == OP_EMIT_STREAM && (len != 3 || t[i + 2] != 0))
                return 0;
            if (!declared || written != declared || emitted == vertices)
                return 0;
            emitted++;
            written = 0;
        }
        else if (op == OP_RET)
            done = 1;
        else
            return 0;
        i += len;
    }
    return done && vertices && emitted == vertices
        && topology == (vertices == 1 ? 1u : 3u);  /* pointlist / linestrip */
}

static int gs_is_pass_through(const void *code, size_t len)
{
    dxbc_chunk body;

    if (!dxbc_find(code, len, "SHDR", &body) && !dxbc_find(code, len, "SHEX", &body))
        return 0;
    return body_is_pass_through(&body) && signatures_identical(code, len);
}

static int gs_tagged_pass_through(void *gs)
{
    unsigned char tag = 0;
    UINT size = sizeof(tag);

    return gs && SUCCEEDED(ID3D11DeviceChild_GetPrivateData((ID3D11DeviceChild *)gs,
                                                            &k_passthrough_tag, &size, &tag))
        && tag;
}

/* Bytes one output vertex of a vertex shader occupies in D3DMetal's stage layout:
 * each signature element is a float/float2/float3/float4 field (float3 takes 16)
 * placed at its natural alignment in signature order.  0 without a readable output
 * signature. */
static UINT vertex_output_bytes(const void *code, size_t len)
{
    dxbc_chunk c;
    UINT entry_size, n, i, bytes = 0;
    sig_entry entries[64];

    if (!output_signature(code, len, &c, &entry_size)
        || !signature_entries(&c, entry_size, entries, &n))
        return 0;
    for (i = 0; i < n; i++)
    {
        UINT m = entries[i].mask & 0xf, comps, size;

        comps = (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1);
        size = comps == 3 ? 16 : comps * 4;
        if (!size)
            continue;
        bytes = (bytes + size - 1) / size * size + size;
    }
    return bytes;
}

/* A pixel shader that declares minimum precision on its SV_Position input deadlocks
 * the host inside Draw: its worker parks in os_sync_wait_on_address and never wakes,
 * so the group stops at that case and the rest never run.  The discriminator is
 * SV_Position and nothing else: the host draws with minimum precision on texcoords. */
static int ps_minprec_on_position(const void *code, size_t size)
{
    dxbc_chunk c;
    UINT n, e;

    /* ISG1 is the input signature that carries a min-precision field per element;
     * plain ISGN has no such field and so cannot describe this shader. */
    if (!dxbc_find(code, size, "ISG1", &c) || c.size < 8)
        return 0;
    memcpy(&n, c.data, 4);
    for (e = 0; e < n; e++)
    {
        const unsigned char *el = c.data + 8 + (size_t)e * 32;
        UINT name_off, minprec;

        if ((size_t)(el - c.data) + 32 > c.size)
            break;
        memcpy(&name_off, el + 4, 4);
        memcpy(&minprec, el + 28, 4);
        if (minprec && name_off < c.size
            && !_strnicmp((const char *)c.data + name_off, "SV_Position", 11))
            return 1;
    }
    return 0;
}

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

static void WINAPI ctx_draw(void *self, UINT vertex_count, UINT start_vertex)
{
    dm_context *w = self;
    fetch_bounds fb;
    DRAW_PROLOGUE(w, g);

    fetch_bounds_enter(w, &fb, 0, start_vertex, vertex_count, 0, 1, 0, 0);
    ((draw_fn)REAL_CTX(self, Draw))(dxcts_real(self), vertex_count, start_vertex);
    fetch_bounds_leave(w, &fb);
    draw_guard_leave(w, &g);
}

static void WINAPI ctx_draw_indexed(void *self, UINT index_count, UINT start_index,
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

static void WINAPI ctx_draw_instanced(void *self, UINT vertex_count, UINT instance_count,
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

static void WINAPI ctx_draw_indexed_instanced(void *self, UINT index_count,
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

static void WINAPI ctx_draw_auto(void *self)
{
    dm_context *w = self;
    DRAW_PROLOGUE(w, g);

    ((void_fn)REAL_CTX(self, DrawAuto))(dxcts_real(self));
    draw_guard_leave(w, &g);
}

static void WINAPI ctx_draw_instanced_indirect(void *self, ID3D11Buffer *args, UINT offset)
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

static void WINAPI ctx_draw_indexed_instanced_indirect(void *self, ID3D11Buffer *args,
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

static void WINAPI ctx_gs_set_shader(void *self, void *gs, void *const *inst, UINT n)
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

static void WINAPI ctx_hs_set_shader(void *self, void *hs, void *const *inst, UINT n)
{
    ((dm_context *)self)->hs_bound = hs != NULL;
    ((set_shader_fn)REAL_CTX(self, HSSetShader))(dxcts_real(self), hs, inst, n);
}

static void WINAPI ctx_ds_set_shader(void *self, void *ds, void *const *inst, UINT n)
{
    ((dm_context *)self)->ds_bound = ds != NULL;
    ((set_shader_fn)REAL_CTX(self, DSSetShader))(dxcts_real(self), ds, inst, n);
}

static void WINAPI ctx_vs_set_shader(void *self, void *vs, void *const *inst, UINT n)
{
    dm_context *w = self;

    w->vs = vs;
    w->vs_sized = NULL;
    ((set_shader_fn)REAL_CTX(self, VSSetShader))(dxcts_real(self), vs, inst, n);
}

static void WINAPI ctx_ia_set_topology(void *self, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    ctx_note_topology(self, topology);
    ((set_topology_fn)REAL_CTX(self, IASetPrimitiveTopology))(dxcts_real(self), topology);
}

static void WINAPI ctx_ia_set_layout(void *self, void *il)
{
    ctx_note_layout(self, il);
    ((set_layout_fn)REAL_CTX(self, IASetInputLayout))(dxcts_real(self), il);
}

static void WINAPI ctx_ia_set_vbs(void *self, UINT start, UINT n, ID3D11Buffer *const *bufs,
                                  const UINT *strides, const UINT *offsets)
{
    ctx_note_vbs(self, start, n, bufs, strides, offsets);
    ((set_vbs_fn)REAL_CTX(self, IASetVertexBuffers))(dxcts_real(self), start, n, bufs,
                                                     strides, offsets);
}

static void WINAPI ctx_ia_set_ib(void *self, ID3D11Buffer *ib, DXGI_FORMAT format, UINT offset)
{
    ctx_note_ib(self, ib, format, offset);
    ((set_ib_fn)REAL_CTX(self, IASetIndexBuffer))(dxcts_real(self), ib, format, offset);
}

static void WINAPI ctx_om_set_blend(void *self, ID3D11BlendState *bs, const FLOAT *factor,
                                    UINT sample_mask)
{
    ctx_note_blend(self, bs, factor, sample_mask);
    ((set_blend_fn)REAL_CTX(self, OMSetBlendState))(dxcts_real(self), bs, factor, sample_mask);
}

static void WINAPI ctx_om_set_rts(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                                  ID3D11DepthStencilView *dsv)
{
    ctx_note_targets(self, n, rtvs);
    ((set_rts_fn)REAL_CTX(self, OMSetRenderTargets))(dxcts_real(self), n, rtvs, dsv);
}

static void WINAPI ctx_om_set_rts_uav(void *self, UINT n, ID3D11RenderTargetView *const *rtvs,
                                      ID3D11DepthStencilView *dsv, UINT uav_start,
                                      UINT n_uavs, ID3D11UnorderedAccessView *const *uavs,
                                      const UINT *counts)
{
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        ctx_note_targets(self, n, rtvs);
    ((set_rts_uav_fn)REAL_CTX(self, OMSetRenderTargetsAndUnorderedAccessViews))(
        dxcts_real(self), n, rtvs, dsv, uav_start, n_uavs, uavs, counts);
}

static void WINAPI ctx_clear_state(void *self)
{
    ((void_fn)REAL_CTX(self, ClearState))(dxcts_real(self));
    ctx_forget_state(self);
}

static void WINAPI ctx_execute_command_list(void *self, void *list, BOOL restore)
{
    ((exec_cmdlist_fn)REAL_CTX(self, ExecuteCommandList))(dxcts_real(self), list, restore);
    /* With RestoreContextState clear, replaying the list leaves the context in its
     * default state; with it set the bindings come back as they were. */
    if (!restore)
        ctx_forget_state(self);
}

static HRESULT WINAPI ctx_finish_command_list(void *self, BOOL restore, void **list)
{
    HRESULT hr = ((finish_cmdlist_fn)REAL_CTX(self, FinishCommandList))(dxcts_real(self),
                                                                       restore, list);

    if (!restore)
        ctx_forget_state(self);
    return hr;
}

static void WINAPI ctx_swap_state(void *self, void *state, void **previous)
{
    ((swap_state_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl,
                                                 SwapDeviceContextState)])(
        dxcts_real(self), state, previous);
    ctx_refresh_state(self);
}

/* --- context wrapper --- */

static HRESULT WINAPI ctx_qi(void *self, const GUID *iid, void **out)
{
    dm_context *w = self;

    if (iid && out && dxcts_wrapper_answers(&w->base, iid, k_contexts,
                                            sizeof(k_contexts) / sizeof(k_contexts[0])))
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
        release(twin);
        dxcts_wrapper_release(device);
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
            release(ctx);
            return w;
        }
    slots = dxcts_widest_slots(ctx, k_contexts, sizeof(k_contexts) / sizeof(k_contexts[0]),
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
        addref(ctx);   /* the wrapper took over the caller's reference; take it back */
        dxcts_wrapper_release(device);
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

/* --- device hooks: textures --- */

/*
 * The host defers destroying a released resource until the next flush, and walks the
 * undrained backlog on every create - so a test that creates and releases textures
 * without ever flushing gets steadily slower and never gives the memory back.
 * Measured on a create/release loop: 20000 textures cost 9.8 ms/1000 and 132 MB of
 * working set undrained, 3.4 ms/1000 and a flat 74 MB with a flush every 256 creates.
 * An immediate context is not free-threaded, so the flush only happens on the thread
 * that created the device - the one the framework renders from.
 */
#define DRAIN_EVERY 256

static void drain_deferred_deletes(dm_device *dw)
{
    ID3D11DeviceContext *ctx = NULL;

    if (!dm_knob_drain_deletes() || GetCurrentThreadId() != dw->context_thread)
        return;
    if (InterlockedIncrement(&dw->creates_since_drain) % DRAIN_EVERY)
        return;
    ID3D11Device_GetImmediateContext((ID3D11Device *)dxcts_real(dw), &ctx);
    if (!ctx)
        return;
    ID3D11DeviceContext_Flush(ctx);
    release(ctx);
}

/* A texture-shaped create refused because Metal cannot make it. */
static int refuse_texture(UINT format, int depth_stencil, UINT samples, void **out)
{
    if (!dm_knob_texture_guard() || dm_format_texture_ok(format, depth_stencil, samples))
        return 0;
    dm_count(&dm_counts.refused_creates);
    if (out)
        *out = NULL;
    return 1;
}

/*
 * A depth-stencil texture is created without its initial data (D3DMetal drops D24S8's
 * and mispacks D32S8's) and filled through UpdateSubresource; a staging one keeps the
 * bytes as its shadow, since D3DMetal has already dropped the stencil.
 */
static HRESULT create_ds_texture(dm_device *dw, create_texture_fn real, const void *desc,
                                 const D3D11_SUBRESOURCE_DATA *init, void **out, int staging)
{
    ID3D11Device *dev = dxcts_real(dw);
    HRESULT hr = real(dev, desc, staging ? init : NULL, out);
    ds_texture dt;
    UINT subs, i;

    if (FAILED(hr) || !*out || !ds_texture_of(*out, &dt))
        return hr;
    subs = dt.shape.mips * dt.shape.slices;
    if (staging)
    {
        size_t total = ds_sub_offset(&dt, subs);
        unsigned char *shadow = malloc(total ? total : 1);

        if (!shadow)
            return hr;
        for (i = 0; i < subs; i++)
        {
            size_t rp, bytes = ds_sub_bytes(&dt, i, &rp);
            size_t srp = init[i].SysMemPitch ? init[i].SysMemPitch : rp;
            UINT y;

            for (y = 0; y < bytes / rp; y++)
                memcpy(shadow + ds_sub_offset(&dt, i) + y * rp,
                       (const unsigned char *)init[i].pSysMem + y * srp, rp);
        }
        ID3D11Resource_SetPrivateData((ID3D11Resource *)*out, &k_ds_shadow_tag, (UINT)total,
                                      shadow);
        free(shadow);
    }
    else
    {
        ID3D11DeviceContext *ctx = NULL;

        ID3D11Device_GetImmediateContext(dev, &ctx);
        if (ctx)
        {
            for (i = 0; i < subs; i++)
            {
                size_t rp;

                ds_sub_bytes(&dt, i, &rp);
                ds_update(dw, ctx, *out, &dt, i, NULL, init[i].pSysMem,
                          init[i].SysMemPitch ? init[i].SysMemPitch : (UINT)rp);
            }
            release(ctx);
        }
    }
    return hr;
}

/* The texture creates share one shape: an R32G32B32 substitution, the refusal, the
 * depth-stencil initial-data detour, then the real create and the drain. */
typedef struct
{
    DXGI_FORMAT format;
    UINT bind, misc, usage, samples;
    UINT width, height, depth, mips, slices;
    int is3d;
} create_shape;

static HRESULT create_texture_common(dm_device *dw, create_texture_fn real,
                                     const void *desc, size_t desc_size,
                                     size_t format_offset, const create_shape *s,
                                     const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    DXGI_FORMAT twin;
    UINT alpha, texel, stencil_at;
    HRESULT hr;

    if (dm_knob_rgb32() && rgb32_twin(s->format, &twin, &alpha))
    {
        unsigned char copy[sizeof(D3D11_TEXTURE3D_DESC1) > sizeof(D3D11_TEXTURE2D_DESC1)
                           ? sizeof(D3D11_TEXTURE3D_DESC1) : sizeof(D3D11_TEXTURE2D_DESC1)];
        rgb32_init packed;
        const D3D11_SUBRESOURCE_DATA *data = init;

        memcpy(copy, desc, desc_size);
        memcpy(copy + format_offset, &twin, sizeof(twin));
        memset(&packed, 0, sizeof(packed));
        if (init)
            data = rgb32_pack_init(&packed, init, s->is3d ? s->mips : s->mips * s->slices,
                                   s->mips, s->width, s->height, s->depth, s->is3d, alpha);
        hr = real(dxcts_real(dw), copy, data, out);
        rgb32_init_free(&packed);
        if (SUCCEEDED(hr) && out && *out)
        {
            rgb32_tag_set(*out, s->format, alpha);
            *out = wrap_texture(*out, s->format);
        }
        return hr;
    }
    if (refuse_texture(s->format, s->bind & D3D11_BIND_DEPTH_STENCIL, s->samples, out))
        return E_INVALIDARG;
    if (dm_knob_depth_stencil_upload() && init && out && !s->is3d
        && dm_ds_layout(s->format, &texel, &stencil_at) && s->samples <= 1
        && !(s->misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                        | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))
        hr = create_ds_texture(dw, real, desc, init, out, s->usage == D3D11_USAGE_STAGING);
    else
        hr = real(dxcts_real(dw), desc, init, out);
    if (SUCCEEDED(hr))
        drain_deferred_deletes(dw);
    return hr;
}

static HRESULT WINAPI dev_create_tex1d(void *self, const D3D11_TEXTURE1D_DESC *desc,
                                       const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    create_texture_fn real = (create_texture_fn)REAL_DEV(self, CreateTexture1D);
    create_shape s;

    if (!desc)
        return real(dxcts_real(self), desc, init, out);
    s.format = desc->Format; s.bind = desc->BindFlags; s.misc = desc->MiscFlags;
    s.usage = desc->Usage; s.samples = 1; s.width = desc->Width; s.height = 1;
    s.depth = 1; s.mips = desc->MipLevels; s.slices = desc->ArraySize; s.is3d = 0;
    return create_texture_common(self, real, desc, sizeof(*desc),
                                 offsetof(D3D11_TEXTURE1D_DESC, Format), &s, init, out);
}

static HRESULT WINAPI dev_create_tex2d(void *self, const D3D11_TEXTURE2D_DESC *desc,
                                       const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    create_texture_fn real = (create_texture_fn)REAL_DEV(self, CreateTexture2D);
    create_shape s;

    if (!desc)
        return real(dxcts_real(self), desc, init, out);
    s.format = desc->Format; s.bind = desc->BindFlags; s.misc = desc->MiscFlags;
    s.usage = desc->Usage; s.samples = desc->SampleDesc.Count; s.width = desc->Width;
    s.height = desc->Height; s.depth = 1; s.mips = desc->MipLevels;
    s.slices = desc->ArraySize; s.is3d = 0;
    return create_texture_common(self, real, desc, sizeof(*desc),
                                 offsetof(D3D11_TEXTURE2D_DESC, Format), &s, init, out);
}

static HRESULT WINAPI dev_create_tex3d(void *self, const D3D11_TEXTURE3D_DESC *desc,
                                       const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    create_texture_fn real = (create_texture_fn)REAL_DEV(self, CreateTexture3D);
    create_shape s;

    if (!desc)
        return real(dxcts_real(self), desc, init, out);
    s.format = desc->Format; s.bind = desc->BindFlags; s.misc = desc->MiscFlags;
    s.usage = desc->Usage; s.samples = 1; s.width = desc->Width; s.height = desc->Height;
    s.depth = desc->Depth; s.mips = desc->MipLevels; s.slices = 1; s.is3d = 1;
    return create_texture_common(self, real, desc, sizeof(*desc),
                                 offsetof(D3D11_TEXTURE3D_DESC, Format), &s, init, out);
}

static HRESULT WINAPI dev_create_tex2d1(void *self, const D3D11_TEXTURE2D_DESC1 *desc,
                                        const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    create_texture_fn real = (create_texture_fn)
        dxcts_real_vtable(self)[SLOT(ID3D11Device3Vtbl, CreateTexture2D1)];
    create_shape s;

    if (!desc)
        return real(dxcts_real(self), desc, init, out);
    s.format = desc->Format; s.bind = desc->BindFlags; s.misc = desc->MiscFlags;
    s.usage = desc->Usage; s.samples = desc->SampleDesc.Count; s.width = desc->Width;
    s.height = desc->Height; s.depth = 1; s.mips = desc->MipLevels;
    s.slices = desc->ArraySize; s.is3d = 0;
    return create_texture_common(self, real, desc, sizeof(*desc),
                                 offsetof(D3D11_TEXTURE2D_DESC1, Format), &s, init, out);
}

static HRESULT WINAPI dev_create_tex3d1(void *self, const D3D11_TEXTURE3D_DESC1 *desc,
                                        const D3D11_SUBRESOURCE_DATA *init, void **out)
{
    create_texture_fn real = (create_texture_fn)
        dxcts_real_vtable(self)[SLOT(ID3D11Device3Vtbl, CreateTexture3D1)];
    create_shape s;

    if (!desc)
        return real(dxcts_real(self), desc, init, out);
    s.format = desc->Format; s.bind = desc->BindFlags; s.misc = desc->MiscFlags;
    s.usage = desc->Usage; s.samples = 1; s.width = desc->Width; s.height = desc->Height;
    s.depth = desc->Depth; s.mips = desc->MipLevels; s.slices = 1; s.is3d = 1;
    return create_texture_common(self, real, desc, sizeof(*desc),
                                 offsetof(D3D11_TEXTURE3D_DESC1, Format), &s, init, out);
}

/* --- device hooks: views --- */

/* D3DMetal renders to textures only: a render-target view of a buffer is built by
 * D3D11Texture::GetView on an object that is not a texture and the process dies. */
static int refuse_buffer_rtv(ID3D11Resource *res, void **out)
{
    ID3D11Buffer *buf = NULL;

    if (!dm_knob_create_guards() || !res
        || FAILED(ID3D11Resource_QueryInterface(res, &k_iid_d3d11_buffer, (void **)&buf))
        || !buf)
        return 0;
    release(buf);
    dm_count(&dm_counts.refused_creates);
    if (out)
        *out = NULL;
    return 1;
}

/* Every view DESC and DESC1 carries the format first, so the twin substitution is one
 * write at offset 0 into a copy of whichever the caller passed. */
static HRESULT create_view(void *self, int slot, ID3D11Resource *res, const void *desc,
                           size_t desc_size, void **out, int rtv)
{
    create_view_fn real = (create_view_fn)dxcts_real_vtable(self)[slot];
    DXGI_FORMAT twin;
    UINT alpha;

    res = unwrap(res);
    if (desc && dm_knob_rgb32() && rgb32_twin(*(const DXGI_FORMAT *)desc, &twin, &alpha)
        && rgb32_tagged(res, NULL))
    {
        unsigned char copy[sizeof(D3D11_UNORDERED_ACCESS_VIEW_DESC1)
                           > sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC1)
                           ? sizeof(D3D11_UNORDERED_ACCESS_VIEW_DESC1)
                           : sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC1)];

        memcpy(copy, desc, desc_size);
        memcpy(copy, &twin, sizeof(twin));
        return real(dxcts_real(self), res, copy, out);
    }
    if (rtv && refuse_buffer_rtv(res, out))
        return E_INVALIDARG;
    return real(dxcts_real(self), res, desc, out);
}

_Static_assert(offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Format) == 0
               && offsetof(D3D11_RENDER_TARGET_VIEW_DESC1, Format) == 0
               && offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC, Format) == 0
               && offsetof(D3D11_SHADER_RESOURCE_VIEW_DESC1, Format) == 0
               && offsetof(D3D11_UNORDERED_ACCESS_VIEW_DESC, Format) == 0
               && offsetof(D3D11_UNORDERED_ACCESS_VIEW_DESC1, Format) == 0,
               "view descriptions no longer start with their format");

#define DEVICE_CREATE_VIEW(fn, vtbl, method, desc_type, rtv)                  \
    static HRESULT WINAPI fn(void *self, ID3D11Resource *res,                 \
                             const desc_type *desc, void **out)               \
    {                                                                         \
        return create_view(self, SLOT(vtbl, method), res, desc, sizeof(*desc), \
                           out, rtv);                                         \
    }

DEVICE_CREATE_VIEW(dev_create_rtv, ID3D11DeviceVtbl, CreateRenderTargetView,
                   D3D11_RENDER_TARGET_VIEW_DESC, 1)
DEVICE_CREATE_VIEW(dev_create_rtv1, ID3D11Device3Vtbl, CreateRenderTargetView1,
                   D3D11_RENDER_TARGET_VIEW_DESC1, 1)
DEVICE_CREATE_VIEW(dev_create_srv, ID3D11DeviceVtbl, CreateShaderResourceView,
                   D3D11_SHADER_RESOURCE_VIEW_DESC, 0)
DEVICE_CREATE_VIEW(dev_create_srv1, ID3D11Device3Vtbl, CreateShaderResourceView1,
                   D3D11_SHADER_RESOURCE_VIEW_DESC1, 0)
DEVICE_CREATE_VIEW(dev_create_uav, ID3D11DeviceVtbl, CreateUnorderedAccessView,
                   D3D11_UNORDERED_ACCESS_VIEW_DESC, 0)
DEVICE_CREATE_VIEW(dev_create_uav1, ID3D11Device3Vtbl, CreateUnorderedAccessView1,
                   D3D11_UNORDERED_ACCESS_VIEW_DESC1, 0)

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
            dm_count(&dm_counts.refused_creates);
            if (out)
                *out = NULL;
            return E_INVALIDARG;
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
            dm_count(&dm_counts.refused_creates);
            if (out)
                *out = NULL;
            return E_INVALIDARG;
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
        dm_count(&dm_counts.refused_creates);
        if (out)
            *out = NULL;
        return E_INVALIDARG;
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
    if (FAILED(((dxcts_qi_fn)(*(void ***)real)[0])(real, &k_iid_d3d11_device, (void **)&dev))
        || !dev)
        return E_NOTIMPL;
    hr = ID3D11Device_QueryInterface(dev, &k_iid_d3d11_device5, (void **)&dev5);
    if (SUCCEEDED(hr) && dev5)
    {
        ID3D11Device_GetImmediateContext(dev, &ctx);
        if (ctx)
            ID3D11DeviceContext_QueryInterface(ctx, &k_iid_d3d11_context4, (void **)&ctx4);
        if (ctx4)
            hr = ID3D11Device5_CreateFence(dev5, 0, D3D11_FENCE_FLAG_NONE, &k_iid_d3d11_fence,
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
    release(fence); release(ctx4); release(ctx); release(dev5); release(dev);
    return hr;
}

static void *wrap_dxgi_device(void *real)
{
    static const dxcts_iface k_dxgi_devices[] = {
        { { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c } },
          WIDTH(IDXGIDeviceVtbl) },
        { { 0x77db970f, 0x6276, 0x48ba, { 0xba,0x28,0x07,0x01,0x43,0xb4,0x39,0x2c } },
          WIDTH(IDXGIDevice1Vtbl) },
        { { 0x05008617, 0xfbfd, 0x4051, { 0xa7,0x90,0x14,0x48,0x84,0xb4,0xf6,0xa9 } },
          WIDTH(IDXGIDevice2Vtbl) },
        { { 0x6007896c, 0x3244, 0x4afd, { 0xbf,0x18,0xa6,0xd3,0xbe,0xda,0x50,0x23 } },
          WIDTH(IDXGIDevice3Vtbl) },
        { { 0x95b4f95f, 0xd8da, 0x4ca4, { 0x9e,0xe6,0x3b,0x76,0xd5,0x96,0x8a,0x10 } },
          WIDTH(IDXGIDevice4Vtbl) },
    };
    static int probed_slots;
    dxcts_wrapper *w = dxcts_wrap(real, dxcts_widest_slots(real, k_dxgi_devices,
                                                           sizeof(k_dxgi_devices)
                                                           / sizeof(k_dxgi_devices[0]),
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
    if (dxcts_wrapper_answers(w, iid, k_devices, sizeof(k_devices) / sizeof(k_devices[0])))
    {
        dxcts_wrapper_addref(w);
        *out = self;
        return S_OK;
    }
    if (dm_knob_enqueue_set_event()
        && (IsEqualGUID(iid, &k_iid_dxgi_device2) || IsEqualGUID(iid, &k_iid_dxgi_device3)
            || IsEqualGUID(iid, &k_iid_dxgi_device4)))
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

        if (FAILED(((dxcts_qi_fn)(*(void ***)adapter)[0])(adapter, &k_iid_dxgi_adapter,
                                                          (void **)&ad)) || !ad)
            return 0;
        if (FAILED(IDXGIAdapter_GetDesc(ad, &desc)))
        {
            release(ad);
            return 0;
        }
        release(ad);
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
    release(w->st_vs); release(w->st_il); release(w->st_dss); release(w->st_rs);
    release(w->base.real);
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
    dm_device *w = (dm_device *)dxcts_wrap(device, dxcts_widest_slots(device, k_devices,
                                                                        sizeof(k_devices)
                                                                        / sizeof(k_devices[0]),
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

/*
 * The host hashes shader bytecode to key its shader cache, and loads and frees
 * bcrypt.dll and crypt32.dll around doing it.  Under Wine each FreeLibrary drops the
 * last reference to the module's unix half, and every load/unload pair makes dyld
 * rebuild the image atlas it publishes for debuggers - an allocation that is never
 * reclaimed (~540 KB per test case, with the case rate decaying in step).  Holding a
 * reference of our own keeps both loaded; deliberately never freed.
 */
static void pin_crypto_modules(void)
{
    static LONG done;

    if (!dm_knob_pin_crypto() || InterlockedCompareExchange(&done, 1, 0) != 0)
        return;
    LoadLibraryA("bcrypt.dll");
    LoadLibraryA("crypt32.dll");
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
    pin_crypto_modules();
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
                release(ctx);
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
    pin_crypto_modules();
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
