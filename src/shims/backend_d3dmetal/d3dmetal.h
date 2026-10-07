/*
 * The D3DMetal backend layer: what Apple's D3DMetal gets wrong, worked around at the
 * D3D11/D3D12 API so the conformance binaries reach a verdict instead of a crash, a
 * wedge or an exhausted GPU - the same layer whether D3DMetal is in the process (a
 * CrossOver bottle) or behind a guest driver (the Neptune VM), so the two arms measure
 * the same thing.
 *
 * Every workaround is a knob, DXCTS_D3DMETAL_<WHAT>, on whenever this layer is
 * selected and off with =0 (or DXCTS_NO_PATCHES=1 for all of them), so what each one
 * is worth can be measured rather than argued about.
 */

#ifndef DXCTS_BACKEND_D3DMETAL_H
#define DXCTS_BACKEND_D3DMETAL_H

#define COBJMACROS
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../../common/backend.h"
#include "../../common/comwrap.h"
#include "../../common/knob.h"

#define SLOT  DXCTS_SLOT
#define WIDTH DXCTS_WIDTH

/* --- knobs (backend.c) --- */

int dm_knob_msaa_levels(void);      /* honest CheckMultisampleQualityLevels answers */
int dm_knob_feature_support(void);  /* CheckFeatureSupport E_INVALIDARG -> unsupported */
int dm_knob_format_support(void);   /* clear capability bits a format cannot have */
int dm_knob_texture_guard(void);    /* refuse textures Metal cannot make */
int dm_knob_create_guards(void);    /* refuse creates D3DMetal dies on */
int dm_knob_draw_guards(void);      /* drop or alter draws D3DMetal dies on */
int dm_knob_fetch_bounds(void);     /* vertex/index fetches past a buffer read zeros */
int dm_knob_rgb32(void);            /* R32G32B32 textures served as RGBA32 twins */
int dm_knob_depth_stencil_upload(void); /* CPU writes keep their stencil plane */
int dm_knob_bc_copy(void);          /* BC -> uncompressed and overlapping copies */
int dm_knob_device_reuse(void);     /* released D3D11 devices are parked and reused */
int dm_knob_drain_deletes(void);    /* flush every 256 texture creates */
int dm_knob_pin_crypto(void);       /* keep bcrypt/crypt32 loaded */
int dm_knob_enqueue_set_event(void);/* IDXGIDevice2::EnqueueSetEvent via a fence */
int dm_knob_minprec_position(void); /* refuse a PS with min-precision SV_Position */
int dm_knob_d3d12_feature_support(void);

/* --- what the layer changed, reported to the runner at process end (backend.c) --- */

typedef struct
{
    LONG dropped_draws;     /* draws not issued */
    LONG altered_draws;     /* draws issued with different state */
    LONG refused_creates;   /* creates answered with a failure instead of forwarded */
    LONG padded_draws;      /* draws issued on a zero-padded copy of a buffer */
} dm_counters;

extern dm_counters dm_counts;

static inline void dm_count(LONG *counter)
{
    InterlockedIncrement(counter);
}

/* --- Metal's capabilities per DXGI format (formats.c) --- */

typedef struct
{
    unsigned char metal;        /* has a Metal pixel format at all */
    unsigned char typeless;
    unsigned char bc;           /* block compressed */
    unsigned char depth;        /* a depth or stencil plane format (any of the family) */
    unsigned char depth_stencil;/* can be bound as a depth-stencil target */
    unsigned char msaa;         /* can carry more than one sample */
    unsigned char blend;        /* can be blended into */
} dm_format;

/* 0 for a format with no Metal representation (DXGI_FORMAT_UNKNOWN included). */
int dm_format_describe(UINT format, dm_format *out);
int dm_format_msaa_ok(UINT format, UINT samples);
int dm_format_texture_ok(UINT format, int depth_stencil, UINT samples);
int dm_format_blendable(UINT format);
UINT dm_format_impossible_support(UINT format);

/* --- shared between the two APIs --- */

/* A stream-output declaration D3DMetal can hold: 53 entries, none of them a gap. */
#define DM_MAX_STREAM_OUTPUT_ENTRIES 53

/* Depth-stencil formats whose CPU uploads D3DMetal handles wrongly (d3d11.c). */
int dm_ds_layout(UINT format, UINT *texel, UINT *stencil_at);

/* --- entry points (d3d11.c, d3d12.c) --- */

HRESULT WINAPI dm_d3d11_create_device(dxcts_d3d11_create_fn real, void *adapter,
                                      UINT driver_type, HMODULE software, UINT flags,
                                      const void *feature_levels,
                                      UINT num_feature_levels, UINT sdk_version,
                                      void **device, UINT *feature_level,
                                      void **context);
HRESULT WINAPI dm_d3d11_create_device_and_swapchain(
    dxcts_d3d11_create_swapchain_fn real, void *adapter, UINT driver_type,
    HMODULE software, UINT flags, const void *feature_levels, UINT num_feature_levels,
    UINT sdk_version, const void *swapchain_desc, void **swapchain, void **device,
    UINT *feature_level, void **context);
HRESULT WINAPI dm_d3d12_create_device(dxcts_d3d12_create_fn real, void *adapter,
                                      UINT min_feature_level, const GUID *iid,
                                      void **device);

#endif
