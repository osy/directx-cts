/*
 * What Metal can make of each DXGI format, as a static table.
 *
 * D3DMetal builds a MTLTextureDescriptor straight from the D3D description and hands
 * it to Metal, whose validation abort()s the process on a description it cannot take
 * - an unrepresentable format (MTLPixelFormatInvalid), a depth-stencil bind on a
 * colour format, a sample count the format does not carry - where a driver would
 * answer E_INVALIDARG.  The layer refuses those before they reach the implementation,
 * and answers the capability queries the same way, so a test skips the combination
 * instead of losing the process.
 *
 * Transcribed from D3DMetal's DXGI -> MTLPixelFormat table and Apple's pixel-format
 * capability tables for the Apple7+ families (M1 and later, the only GPUs D3DMetal
 * runs on).  Every _TYPELESS maps to its Uint flavour, as in D3DMetal; the D24 family
 * lands on Depth32Float_Stencil8, the format Apple silicon substitutes for it.  Apple
 * GPUs take 1, 2 and 4 samples.  D3DMetal also accepts a request for 3 and makes the
 * texture with 4 - the one non-power-of-two count it serves - so 3 is answered as 4.
 */

#include "d3dmetal.h"

enum
{
    F_METAL    = 0x01,
    F_TYPELESS = 0x02,
    F_BC       = 0x04,
    F_DEPTH    = 0x08,
    F_DS       = 0x10,
    F_MSAA     = 0x20,
    F_BLEND    = 0x40,
};

#define COLOUR_BLEND   (F_METAL | F_MSAA | F_BLEND)
#define COLOUR_INT     (F_METAL | F_MSAA)
#define TYPELESS_INT   (F_METAL | F_TYPELESS | F_MSAA)
#define DEPTH          (F_METAL | F_DEPTH | F_DS | F_MSAA)
#define DEPTH_TYPELESS (F_METAL | F_TYPELESS | F_DEPTH | F_DS | F_MSAA)
#define BC             (F_METAL | F_BC)
#define BC_TYPELESS    (F_METAL | F_TYPELESS | F_BC)

static unsigned format_flags(UINT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return TYPELESS_INT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return COLOUR_BLEND;
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:     return COLOUR_INT;
    /* Metal has no 96-bit format; D3DMetal widens these to the RGBA32 twin (and the
     * layer's rgb32 workaround does the same at the API). */
    case DXGI_FORMAT_R32G32B32_TYPELESS:    return TYPELESS_INT;
    case DXGI_FORMAT_R32G32B32_FLOAT:       return COLOUR_BLEND;
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:        return COLOUR_INT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return TYPELESS_INT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:    return COLOUR_BLEND;
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:     return COLOUR_INT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return TYPELESS_INT;
    case DXGI_FORMAT_R32G32_FLOAT:          return COLOUR_BLEND;
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:           return COLOUR_INT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:     return DEPTH_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return DEPTH;
    /* Typed single-plane views despite the name, sampled like the D24 ones below. */
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return DEPTH;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return TYPELESS_INT;
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return COLOUR_BLEND;
    case DXGI_FORMAT_R10G10B10A2_UINT:      return COLOUR_INT;
    case DXGI_FORMAT_R11G11B10_FLOAT:       return COLOUR_BLEND;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return TYPELESS_INT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:        return COLOUR_BLEND;
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:         return COLOUR_INT;
    case DXGI_FORMAT_R16G16_TYPELESS:       return TYPELESS_INT;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:          return COLOUR_BLEND;
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:           return COLOUR_INT;
    /* The typeless parent of a depth family binds as depth-stencil (the usual
     * R32_TYPELESS texture viewed as D32_FLOAT + R32_FLOAT). */
    case DXGI_FORMAT_R32_TYPELESS:          return TYPELESS_INT | F_DS;
    case DXGI_FORMAT_D32_FLOAT:             return DEPTH;
    case DXGI_FORMAT_R32_FLOAT:             return COLOUR_BLEND;
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:              return COLOUR_INT;
    case DXGI_FORMAT_R24G8_TYPELESS:        return DEPTH_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:  return DEPTH;
    case DXGI_FORMAT_R8G8_TYPELESS:         return TYPELESS_INT;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:            return COLOUR_BLEND;
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:             return COLOUR_INT;
    case DXGI_FORMAT_R16_TYPELESS:          return TYPELESS_INT | F_DS;
    case DXGI_FORMAT_R16_FLOAT:             return COLOUR_BLEND;
    case DXGI_FORMAT_D16_UNORM:             return DEPTH;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:             return COLOUR_BLEND;
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT:              return COLOUR_INT;
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:        return COLOUR_BLEND;
    case DXGI_FORMAT_R8_TYPELESS:           return TYPELESS_INT;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:              return COLOUR_BLEND;
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:               return COLOUR_INT;
    case DXGI_FORMAT_A8_UNORM:              return COLOUR_BLEND;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:    return COLOUR_BLEND;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:   return COLOUR_BLEND;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return F_METAL | F_TYPELESS | F_MSAA | F_BLEND;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:          return BC_TYPELESS;
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:        return BC;
    /* Everything else - the planar and packed video formats, the palettized ones,
     * R1_UNORM, R10G10B10_XR_BIAS_A2_UNORM, the 4:2:2 pair and UNKNOWN - has no Metal
     * equivalent. */
    default:                                return 0;
    }
}

int dm_format_describe(UINT format, dm_format *out)
{
    unsigned f = format_flags(format);

    memset(out, 0, sizeof(*out));
    if (!(f & F_METAL))
        return 0;
    out->metal = 1;
    out->typeless = (f & F_TYPELESS) != 0;
    out->bc = (f & F_BC) != 0;
    out->depth = (f & F_DEPTH) != 0;
    out->depth_stencil = (f & F_DS) != 0;
    out->msaa = (f & F_MSAA) != 0;
    out->blend = (f & F_BLEND) != 0;
    return 1;
}

int dm_format_msaa_ok(UINT format, UINT samples)
{
    dm_format d;

    if (!dm_format_describe(format, &d))
        return 0;
    if (samples == 1)
        return 1;
    if (!d.msaa)
        return 0;
    if (samples == 3)
        samples = 4;
    return samples == 2 || samples == 4;
}

int dm_format_texture_ok(UINT format, int depth_stencil, UINT samples)
{
    dm_format d;

    if (!dm_format_describe(format, &d))
        return 0;
    if (depth_stencil && !d.depth_stencil)
        return 0;
    return samples <= 1 || dm_format_msaa_ok(format, samples);
}

/* Unknown to the table means "not ours to guard": only a format Metal is known not to
 * blend counts as unblendable. */
int dm_format_blendable(UINT format)
{
    dm_format d;

    return !dm_format_describe(format, &d) || d.blend;
}

/*
 * DXGI format classes whose capabilities D3D11 fixes regardless of implementation.
 * The lists are the ones DXGI_FORMAT itself defines, asserted against the header so a
 * renumbered or extended enum is a build failure rather than a silent gap.
 */
static int format_is_block_compressed(UINT format)
{
    return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM)
        || (format >= DXGI_FORMAT_BC6H_TYPELESS
            && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

_Static_assert(DXGI_FORMAT_BC1_TYPELESS == 70 && DXGI_FORMAT_BC5_SNORM == 84
               && DXGI_FORMAT_BC6H_TYPELESS == 94
               && DXGI_FORMAT_BC7_UNORM_SRGB == 99,
               "the block-compressed formats are no longer two contiguous ranges");

/*
 * Every member of a depth-stencil format family: the D* formats themselves and the
 * typeless and single-plane view formats that share their bit layout.  D3D11 defines
 * none of them as a render target, a blend target, a scanout format, or a buffer or
 * input-assembler format, and answering otherwise is what wedges or aborts the host.
 * Blend\Format\R32_FLOAT_X8X24_TYPELESS spun at ~178% CPU with no further events and
 * becomes a 5 s Skipped case; WGF11ResourceFormats\Formats believed it could make a
 * typed buffer of one, which the host has no Metal pixel format for - it builds a
 * descriptor with pixelFormat 0, and Metal's validation aborts the process.
 *
 * R32_TYPELESS and R16_TYPELESS are deliberately absent: they are colour formats a
 * depth view can also be created on, so they can be render targets.
 */
static int format_is_depth_family(UINT format)
{
    dm_format d;

    return dm_format_describe(format, &d) && d.depth;
}

/*
 * None of these classes is a buffer or an input-assembler format.  Answering otherwise
 * is what let WGF11ResourceFormats\Formats try to build a typed buffer of a format the
 * host has no pixel format for, so all three clear the same four bits rather than only
 * the depth family that first showed the failure.
 */
#define FORMAT_NOT_A_BUFFER                                                   \
    (D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER      \
     | D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER | D3D11_FORMAT_SUPPORT_SO_BUFFER)

#define FORMAT_NOT_A_RENDER_TARGET                                            \
    (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE      \
     | D3D11_FORMAT_SUPPORT_DISPLAY)

/* Capabilities the format cannot have, whatever the implementation answered. */
UINT dm_format_impossible_support(UINT format)
{
    dm_format d;
    UINT mask = 0;

    if (format_is_block_compressed(format))
        mask |= FORMAT_NOT_A_RENDER_TARGET | FORMAT_NOT_A_BUFFER
              | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL
              | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN
              | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE
              | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    /* A typeless format cannot be read by a shader either: a view onto one has to name
     * a typed format, so ld, sample and gather are all disallowed. */
    if (dm_format_describe(format, &d) && d.typeless)
        mask |= FORMAT_NOT_A_RENDER_TARGET | FORMAT_NOT_A_BUFFER
              | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL
              | D3D11_FORMAT_SUPPORT_SHADER_LOAD
              | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
              | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON
              | D3D11_FORMAT_SUPPORT_SHADER_GATHER
              | D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON
              | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    /* A depth-stencil format cannot be a volume texture and cannot have its mips
     * generated: autogen needs the render-target and shader-sample pair that the
     * class does not have.  The host answers otherwise, and WGF11Filter\MipGen\3D
     * then asks it to generate mips for a 3D R32_FLOAT_X8X24_TYPELESS texture, which
     * faults inside Metal's blit encoder on a libdispatch thread - unrecoverable, so
     * the process spins in the signal handler instead of finishing the group.  WARP
     * skips 22 of that group's 52 cases; this is what lets it. */
    if (format_is_depth_family(format))
        mask |= FORMAT_NOT_A_RENDER_TARGET | FORMAT_NOT_A_BUFFER
              | D3D11_FORMAT_SUPPORT_TEXTURE3D
              | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    return mask;
}
