/*
 * Resources the host gets wrong: R32G32B32 textures served as RGBA32 twins, the
 * stencil plane of CPU writes into depth-stencil textures, and the copies D3DMetal
 * cannot encode.
 */

#include "d3d11_layer.h"

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

int rgb32_view(ID3D11View *view)
{
    ID3D11Resource *res = NULL;
    int tagged;

    if (!view)
        return 0;
    ID3D11View_GetResource(view, &res);
    if (!res)
        return 0;
    tagged = rgb32_tagged(res, NULL);
    dxcts_release(res);
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
    { &IID_ID3D11Texture1D, WIDTH(ID3D11Texture1DVtbl) },
};
static const dxcts_iface k_textures2d[] = {
    { &IID_ID3D11Texture2D, WIDTH(ID3D11Texture2DVtbl) },
    { &IID_ID3D11Texture2D1, WIDTH(ID3D11Texture2D1Vtbl) },
};
static const dxcts_iface k_textures3d[] = {
    { &IID_ID3D11Texture3D, WIDTH(ID3D11Texture3DVtbl) },
    { &IID_ID3D11Texture3D1, WIDTH(ID3D11Texture3D1Vtbl) },
};

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
        set = k_textures1d; count = DXCTS_COUNT(k_textures1d);
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        set = k_textures3d; count = DXCTS_COUNT(k_textures3d);
    }
    else
    {
        set = k_textures2d; count = DXCTS_COUNT(k_textures2d);
    }
    if (IsEqualGUID(iid, &IID_ID3D11Resource) || IsEqualGUID(iid, &IID_ID3D11DeviceChild)
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
        slots = dxcts_widest_slots(tex, k_textures1d, DXCTS_COUNT(k_textures1d), &probed_1d);
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        slots = dxcts_widest_slots(tex, k_textures3d, DXCTS_COUNT(k_textures3d), &probed_3d);
    else
        slots = dxcts_widest_slots(tex, k_textures2d, DXCTS_COUNT(k_textures2d), &probed_2d);
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

/* The two depth-stencil layouts D3DMetal uploads wrongly: bytes per texel and the
 * offset of the stencil byte within one. */
static int ds_layout(UINT format, UINT *texel, UINT *stencil_at)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        *texel = 8; *stencil_at = 4;
        return 1;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        *texel = 4; *stencil_at = 3;
        return 1;
    default:
        return 0;
    }
}

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
    return ds_layout(t->shape.format, &t->texel, &t->stencil_at);
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
        dxcts_release(err);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &dw->st_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(dev, &k_elem, 1,
                                            ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), &dw->st_il);
    dxcts_release(code);
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
        dxcts_release(dw->st_vs); dxcts_release(dw->st_il); dxcts_release(dw->st_dss); dxcts_release(dw->st_rs);
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
    dxcts_release(s->vs); dxcts_release(s->ps); dxcts_release(s->gs); dxcts_release(s->hs); dxcts_release(s->ds);
    dxcts_release(s->il); dxcts_release(s->vb); dxcts_release(s->rs);
    for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        dxcts_release(s->rtv[i]);
    dxcts_release(s->dsv); dxcts_release(s->dss); dxcts_release(s->bs);
    for (i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT; i++)
        dxcts_release(s->so[i]);
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
        dxcts_release(vb); dxcts_release(dsv);
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
    dxcts_release(vb);
    dxcts_release(dsv);
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
        dxcts_release(staging);
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
    dxcts_release(staging);
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
        dxcts_release(target);
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
    dxcts_release(tmp);
    return 1;
}

/* --- context hooks: copies and CPU byte paths --- */

ID3D11Device *ctx_real_device(dm_context *w)
{
    return (ID3D11Device *)dxcts_real(w->device);
}

void WINAPI ctx_copy_resource(void *self, ID3D11Resource *dst, ID3D11Resource *src)
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

void WINAPI ctx_copy_region(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
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

void WINAPI ctx_copy_region1(void *self, ID3D11Resource *dst, UINT dst_sub, UINT x,
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

/* On a deferred context of a driver without command lists, Microsoft's runtime adds the
 * destination box's offset to pSrcData (documented under
 * ID3D11DeviceContext::UpdateSubresource), so applications pass pSrcData moved back by
 * that offset, counted in their own texel size.  1 when that applies. */
static int update_src_offset_quirk(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                   const D3D11_BOX *box)
{
    D3D11_FEATURE_DATA_THREADING th;

    if (!box || (!box->left && !box->top && !box->front)
        || ID3D11DeviceContext_GetType(ctx) != D3D11_DEVICE_CONTEXT_DEFERRED)
        return 0;
    if (FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_THREADING, &th,
                                                sizeof(th))))
        return 0;
    return !th.DriverCommandLists;
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
    size_t srow, sslice, drow, dslice, back = 0;
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
    /* Under the runtime's offset quirk the application moved src back in 12-byte
     * texels and the runtime moves the packed data forward in 16-byte ones: find the
     * box data, and lead the packed buffer with the runtime's offset. */
    if (update_src_offset_quirk(ctx_real_device(w), real, box))
    {
        src = (const unsigned char *)src + box->front * sslice + box->top * srow
              + (size_t)box->left * 12;
        back = box->front * dslice + box->top * drow + (size_t)box->left * 16;
    }
    packed = malloc(back + dslice * slices);
    if (!packed)
        return 0;
    rgb32_pack(packed + back, drow, dslice, src, srow, sslice, wd, rows, slices,
               t.alpha_bits);
    if (has_flags)
        ((update1_fn)dxcts_real_vtable(w)[SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1)])(
            real, dst, sub, box, packed, (UINT)drow, (UINT)dslice, flags);
    else
        ID3D11DeviceContext_UpdateSubresource(real, dst, sub, box, packed, (UINT)drow,
                                              (UINT)dslice);
    free(packed);
    return 1;
}

void WINAPI ctx_update(void *self, ID3D11Resource *dst, UINT sub,
                       const D3D11_BOX *box, const void *src, UINT src_row,
                       UINT src_depth)
{
    dst = unwrap(dst);
    if (update_served(self, dst, sub, box, src, src_row, src_depth, 0, 0))
        return;
    ((update_fn)REAL_CTX(self, UpdateSubresource))(dxcts_real(self), dst, sub, box, src,
                                                   src_row, src_depth);
}

void WINAPI ctx_update1(void *self, ID3D11Resource *dst, UINT sub,
                        const D3D11_BOX *box, const void *src, UINT src_row,
                        UINT src_depth, UINT flags)
{
    dst = unwrap(dst);
    if (update_served(self, dst, sub, box, src, src_row, src_depth, flags, 1))
        return;
    ((update1_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl, UpdateSubresource1)])(
        dxcts_real(self), dst, sub, box, src, src_row, src_depth, flags);
}

HRESULT WINAPI ctx_map(void *self, ID3D11Resource *res, UINT sub, D3D11_MAP type,
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

void WINAPI ctx_unmap(void *self, ID3D11Resource *res, UINT sub)
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

void WINAPI ctx_resolve(void *self, ID3D11Resource *dst, UINT dst_sub,
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
void WINAPI ctx_clear_rtv(void *self, ID3D11RenderTargetView *rtv, const FLOAT color[4])
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

void WINAPI ctx_discard_resource(void *self, ID3D11Resource *res)
{
    ((discard_fn)dxcts_real_vtable(self)[SLOT(ID3D11DeviceContext1Vtbl, DiscardResource)])(
        dxcts_real(self), unwrap(res));
}

void WINAPI ctx_set_min_lod(void *self, ID3D11Resource *res, FLOAT lod)
{
    ((set_minlod_fn)REAL_CTX(self, SetResourceMinLOD))(dxcts_real(self), unwrap(res), lod);
}

FLOAT WINAPI ctx_get_min_lod(void *self, ID3D11Resource *res)
{
    return ((get_minlod_fn)REAL_CTX(self, GetResourceMinLOD))(dxcts_real(self), unwrap(res));
}

HRESULT WINAPI dev_create_dsv(void *self, ID3D11Resource *res,
                              const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, void **out)
{
    return ((create_dsv_fn)REAL_DEV(self, CreateDepthStencilView))(dxcts_real(self),
                                                                   unwrap(res), desc, out);
}

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
    dxcts_release(ctx);
}

/* A texture-shaped create refused because Metal cannot make it. */
static int refuse_texture(UINT format, int depth_stencil, UINT samples, void **out)
{
    if (!dm_knob_texture_guard() || dm_format_texture_ok(format, depth_stencil, samples))
        return 0;
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
            dxcts_release(ctx);
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
        && ds_layout(s->format, &texel, &stencil_at) && s->samples <= 1
        && !(s->misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                        | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))
        hr = create_ds_texture(dw, real, desc, init, out, s->usage == D3D11_USAGE_STAGING);
    else
        hr = real(dxcts_real(dw), desc, init, out);
    if (SUCCEEDED(hr))
        drain_deferred_deletes(dw);
    return hr;
}

HRESULT WINAPI dev_create_tex1d(void *self, const D3D11_TEXTURE1D_DESC *desc,
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

HRESULT WINAPI dev_create_tex2d(void *self, const D3D11_TEXTURE2D_DESC *desc,
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

HRESULT WINAPI dev_create_tex3d(void *self, const D3D11_TEXTURE3D_DESC *desc,
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

HRESULT WINAPI dev_create_tex2d1(void *self, const D3D11_TEXTURE2D_DESC1 *desc,
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

HRESULT WINAPI dev_create_tex3d1(void *self, const D3D11_TEXTURE3D_DESC1 *desc,
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
        || FAILED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Buffer, (void **)&buf))
        || !buf)
        return 0;
    dxcts_release(buf);
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
    HRESULT WINAPI fn(void *self, ID3D11Resource *res,                 \
                      const desc_type *desc, void **out)                      \
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

