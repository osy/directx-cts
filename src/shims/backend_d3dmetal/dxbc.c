/*
 * Reading shader bytecode: the signatures and token streams the layer recognises a
 * shader by.
 */

#include "d3d11_layer.h"

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
 * Detection reads the SM4/SM5 token stream: input primitive point, line or triangle,
 * the matching list or strip topology, and a body that is nothing but, for each input
 * vertex in order, a copy of every declared output component from the same component
 * of the same register of that vertex - whole registers or one component at a time,
 * as fxc writes a register that packs several elements - followed by one emit, then
 * ret.  The input and output signatures must agree register for register.  Anything
 * else - a swizzle, a modifier, an arithmetic op, an extra emit, a constant, a DXIL
 * body - leaves the shader alone.
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
    UINT vertices = 0, topology = 0, emitted = 0;
    UINT declared[32] = {0}, written[32] = {0};     /* component masks per register */
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

                vertices = prim >= 1 && prim <= 3 ? prim : 0;  /* point, line, triangle */
                if (!vertices)
                    return 0;
            }
            else if (op == OP_DCL_OUTPUT_TOPOLOGY)
                topology = (tok >> 11) & 0x7f;
            else if (op == OP_DCL_OUTPUT || op == OP_DCL_OUTPUT_SGV || op == OP_DCL_OUTPUT_SIV)
            {
                UINT dcl = len >= 3 ? t[i + 1] : 0;

                if (len < 3 || !plain_operand(dcl, OPERAND_OUTPUT, 1) || ((dcl >> 2) & 3) != 0
                    || t[i + 2] >= 32)
                    return 0;
                declared[t[i + 2]] |= (dcl >> 4) & 0xf;
            }
        }
        else if (op == OP_MOV)
        {
            /* mov oN.mask, v[k][N] with each written component read from itself, through
             * an identity swizzle or a single-component select -- no saturate, nothing
             * extended */
            UINT dst, reg, src, mask, comp;

            if (len != 6 || (tok >> 11) & 0x1fff)
                return 0;
            dst = t[i + 1]; reg = t[i + 2]; src = t[i + 3];
            if (!plain_operand(dst, OPERAND_OUTPUT, 1) || ((dst >> 2) & 3) != 0 || reg >= 32)
                return 0;
            mask = (dst >> 4) & 0xf;
            if (!mask || (mask & ~declared[reg]) || (mask & written[reg]))
                return 0;
            if (!plain_operand(src, OPERAND_INPUT, 2) || t[i + 4] != emitted || t[i + 5] != reg)
                return 0;
            if (((src >> 2) & 3) == 1)
            {
                for (comp = 0; comp < 4; comp++)
                    if ((mask & (1u << comp)) && ((src >> (4 + 2 * comp)) & 3) != comp)
                        return 0;
            }
            else if (((src >> 2) & 3) != 2 || mask != 1u << ((src >> 4) & 3))
                return 0;
            written[reg] |= mask;
        }
        else if (op == OP_EMIT || op == OP_EMIT_STREAM)
        {
            UINT r, any = 0;

            if (op == OP_EMIT_STREAM && (len != 3 || t[i + 2] != 0))
                return 0;
            if (emitted == vertices)
                return 0;
            for (r = 0; r < 32; r++)
            {
                if (written[r] != declared[r])
                    return 0;
                any |= declared[r];
                written[r] = 0;
            }
            if (!any)
                return 0;
            emitted++;
        }
        else if (op == OP_RET)
            done = 1;
        else
            return 0;
        i += len;
    }
    /* pointlist / linestrip / trianglestrip */
    return done && vertices && emitted == vertices && topology == 2 * vertices - 1;
}

int gs_is_pass_through(const void *code, size_t len)
{
    dxbc_chunk body;

    if (!dxbc_find(code, len, "SHDR", &body) && !dxbc_find(code, len, "SHEX", &body))
        return 0;
    return body_is_pass_through(&body) && signatures_identical(code, len);
}

int gs_tagged_pass_through(void *gs)
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
UINT vertex_output_bytes(const void *code, size_t len)
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
int ps_minprec_on_position(const void *code, size_t size)
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

