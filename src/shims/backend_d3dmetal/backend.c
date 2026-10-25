/*
 * The D3DMetal backend layer's exports, knobs and end-of-process report.  See
 * d3dmetal.h for what the layer is and common/backend.h for how a shim reaches it.
 */

#include "d3dmetal.h"

#include "../../common/events.h"
#include "../../common/shim.h"

#define BACKEND_API __declspec(dllexport) WINAPI

/* On whenever this layer is selected, on either platform: selecting the layer is the
 * platform judgement, and each knob is only a way of measuring one workaround. */
#define DM_KNOB(fn, name) DXCTS_KNOB(fn##_knob, name, 1, 1) \
    int fn(void) { return fn##_knob(); }

DM_KNOB(dm_knob_msaa_levels,          "DXCTS_D3DMETAL_MSAA_QUALITY_LEVELS")
DM_KNOB(dm_knob_feature_support,      "DXCTS_D3DMETAL_FEATURE_SUPPORT")
DM_KNOB(dm_knob_format_support,       "DXCTS_D3DMETAL_FORMAT_SUPPORT")
DM_KNOB(dm_knob_texture_guard,        "DXCTS_D3DMETAL_TEXTURE_GUARD")
DM_KNOB(dm_knob_create_guards,        "DXCTS_D3DMETAL_CREATE_GUARDS")
DM_KNOB(dm_knob_draw_guards,          "DXCTS_D3DMETAL_DRAW_GUARDS")
DM_KNOB(dm_knob_fetch_bounds,         "DXCTS_D3DMETAL_FETCH_BOUNDS")
DM_KNOB(dm_knob_rgb32,                "DXCTS_D3DMETAL_RGB32")
DM_KNOB(dm_knob_depth_stencil_upload, "DXCTS_D3DMETAL_DEPTH_STENCIL_UPLOAD")
DM_KNOB(dm_knob_bc_copy,              "DXCTS_D3DMETAL_BC_COPY")
DM_KNOB(dm_knob_device_reuse,         "DXCTS_D3DMETAL_DEVICE_REUSE")
DM_KNOB(dm_knob_drain_deletes,        "DXCTS_D3DMETAL_DRAIN_DELETES")
DM_KNOB(dm_knob_enqueue_set_event,    "DXCTS_D3DMETAL_ENQUEUE_SET_EVENT")
DM_KNOB(dm_knob_minprec_position,     "DXCTS_D3DMETAL_MINPREC_SV_POSITION")
DM_KNOB(dm_knob_d3d12_feature_support,"DXCTS_D3DMETAL_D3D12_FEATURE_SUPPORT")

dm_counters dm_counts;

HRESULT BACKEND_API dxcts_backend_d3d11_create_device(
    dxcts_d3d11_create_fn real, void *adapter, UINT driver_type, HMODULE software,
    UINT flags, const void *feature_levels, UINT num_feature_levels, UINT sdk_version,
    void **device, UINT *feature_level, void **context)
{
    return dm_d3d11_create_device(real, adapter, driver_type, software, flags,
                                  feature_levels, num_feature_levels, sdk_version,
                                  device, feature_level, context);
}

HRESULT BACKEND_API dxcts_backend_d3d11_create_device_and_swapchain(
    dxcts_d3d11_create_swapchain_fn real, void *adapter, UINT driver_type,
    HMODULE software, UINT flags, const void *feature_levels, UINT num_feature_levels,
    UINT sdk_version, const void *swapchain_desc, void **swapchain, void **device,
    UINT *feature_level, void **context)
{
    return dm_d3d11_create_device_and_swapchain(real, adapter, driver_type, software,
                                                flags, feature_levels,
                                                num_feature_levels, sdk_version,
                                                swapchain_desc, swapchain, device,
                                                feature_level, context);
}

HRESULT BACKEND_API dxcts_backend_d3d12_create_device(dxcts_d3d12_create_fn real,
                                                      void *adapter,
                                                      UINT min_feature_level,
                                                      const GUID *iid, void **device)
{
    return dm_d3d12_create_device(real, adapter, min_feature_level, iid, device);
}

/*
 * The workarounds this layer applies, for the report's `workarounds` array: one entry
 * per knob, with whether it is on in this run, so the description lives with the code
 * it describes.
 */
typedef struct
{
    const char *name;
    int (*active)(void);
    const char *what;
    const char *why;
    int alters_results;
} workaround;

static const workaround k_workarounds[] = {
    { "d3dmetal/msaa-quality-levels", dm_knob_msaa_levels,
      "answers CheckMultisampleQualityLevels S_OK with zero levels for a count or "
      "format Metal cannot carry, and for the counts the host answers E_INVALIDARG",
      "the host answers one level for any count on any format, BC included, and "
      "Metal aborts on the texture a test then creates", 0 },
    { "d3dmetal/feature-support", dm_knob_feature_support,
      "answers CheckFeatureSupport S_OK and unsupported where the host returns "
      "E_INVALIDARG for a feature it does not know",
      "several binaries query one from InitTestGroups and abandon setup", 0 },
    { "d3dmetal/format-support", dm_knob_format_support,
      "clears the capabilities a format class cannot have from CheckFormatSupport "
      "and CheckFeatureSupport answers",
      "the host reports 0xffffffff for every format, so a test otherwise asks it "
      "for operations no implementation can perform and it faults", 0 },
    { "d3dmetal/texture-guard", dm_knob_texture_guard,
      "refuses a texture create, or a D3D12 allocation-info query, of a format, "
      "depth-stencil bind or sample count Metal cannot make",
      "D3DMetal hands the descriptor to Metal, whose validation aborts the process", 0 },
    { "d3dmetal/create-guards", dm_knob_create_guards,
      "refuses render-target views of buffers, input layouts of 32 elements or with "
      "an unnamed element, stream-output declarations with gaps or over 53 entries; "
      "drops render-target barriers of D3D12 buffers and indexed draws recorded with "
      "no index buffer",
      "each of those dies inside D3DMetal (strdup of NULL, a table overrun, texture "
      "selectors sent to a buffer, a nil index buffer in the GPU driver)", 0 },
    { "d3dmetal/draw-guards", dm_knob_draw_guards,
      "never binds a pass-through geometry shader (refuses a D3D12 pipeline state "
      "with any geometry shader beside tessellation), skips a tessellated draw over "
      "D3DMetal's 0x3b80-byte patch budget, turns alpha-to-coverage off on a target "
      "Metal cannot blend, and skips a MIN or MAX resolve of a depth resource where "
      "Metal would abort on it",
      "D3DMetal accepts each at bind time and aborts or faults when it encodes the "
      "draw", 1 },
    { "d3dmetal/fetch-bounds", dm_knob_fetch_bounds,
      "issues a draw whose vertex or index fetches reach past a bound buffer on a "
      "zero-padded copy of it",
      "D3D reads such fetches as zeros; D3DMetal reads the neighbouring allocation", 0 },
    { "d3dmetal/rgb32", dm_knob_rgb32,
      "creates R32G32B32 textures as their RGBA32 twin and repacks every CPU-visible "
      "byte path",
      "Metal has no 96-bit format and every D3DMetal upload of one is a blit AGX "
      "rejects, leaving the texture zeroed", 0 },
    { "d3dmetal/depth-stencil-upload", dm_knob_depth_stencil_upload,
      "rewrites the stencil plane after every CPU write into a depth-stencil texture, "
      "with stencil-replace point draws",
      "D3DMetal writes only the depth plane from initial data, UpdateSubresource and "
      "staging maps", 0 },
    { "d3dmetal/bc-copy", dm_knob_bc_copy,
      "serves block-compressed to uncompressed copies through a staging readback, and "
      "overlapping same-resource 3D copies through a temporary",
      "the first dies in the GPU driver; the second is written slice by slice, "
      "reading what was just written", 0 },
    { "d3dmetal/device-reuse", dm_knob_device_reuse,
      "parks a released D3D11 device and serves the next create with the same "
      "arguments from it",
      "D3DMetal never frees a device: 256 MiB per create/release cycle, and the "
      "HLK creates one per case", 0 },
    { "d3dmetal/drain-deletes", dm_knob_drain_deletes,
      "flushes the immediate context every 256 texture creates",
      "the host defers destroying released resources until a flush and walks the "
      "backlog on every create", 0 },
    { "d3dmetal/enqueue-set-event", dm_knob_enqueue_set_event,
      "implements IDXGIDevice2::EnqueueSetEvent with a fence where the host answers "
      "E_NOTIMPL",
      "the framework's BlockingFinish waits on it before testing anything", 0 },
    { "d3dmetal/minprec-sv-position", dm_knob_minprec_position,
      "refuses a pixel shader declaring minimum precision on SV_Position",
      "the host deadlocks inside the draw that uses one", 0 },
    { "d3dmetal/d3d12-feature-support", dm_knob_d3d12_feature_support,
      "answers D3D12 CheckFeatureSupport S_OK and unsupported where the host returns "
      "E_INVALIDARG",
      "a conformance binary that asks about one dies with no case run", 0 },
};

/* The i-th workaround, for the runner's report; 0 past the end. */
int BACKEND_API dxcts_backend_workaround(int i, const char **name, int *active,
                                         const char **what, const char **why,
                                         int *alters_results)
{
    const workaround *w;

    if (i < 0 || (size_t)i >= sizeof(k_workarounds) / sizeof(k_workarounds[0]))
        return 0;
    w = &k_workarounds[i];
    *name = w->name;
    *active = w->active();
    *what = w->what;
    *why = w->why;
    *alters_results = w->alters_results;
    return 1;
}

/* Runs at process detach, so kernel32 only: the CRT may already be gone. */
static void report_counter(HANDLE fh, const char *name, LONG count)
{
    char line[160];
    size_t n = 0;

    if (!count)
        return;
    dxcts_events_append_str(line, sizeof(line), &n,
                            "{\"event\":\"workaround\",\"name\":\"");
    dxcts_events_append_str(line, sizeof(line), &n, name);
    dxcts_events_append_str(line, sizeof(line), &n, "\",\"count\":");
    dxcts_events_append_ulong(line, sizeof(line), &n, (unsigned long)count);
    dxcts_events_append_str(line, sizeof(line), &n, "}\n");
    dxcts_events_write(fh, line, (DWORD)n);
}

/*
 * What altered a verdict, counted into the runner's per-group altered_draws.  A group
 * the runner kills at its timeout receives no DLL_PROCESS_DETACH, so its counts go
 * unreported; nothing inside the process can change that.  The refusals and padded
 * draws are not here: a refused create fails the case on its own terms and a padded
 * draw reads what D3D defines.
 */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        dxcts_shim_attach(inst);
    else if (reason == DLL_PROCESS_DETACH
             && (dm_counts.dropped_draws || dm_counts.altered_draws))
    {
        HANDLE fh = dxcts_events_open();

        if (fh != INVALID_HANDLE_VALUE)
        {
            report_counter(fh, "d3dmetal/dropped-draws", dm_counts.dropped_draws);
            report_counter(fh, "d3dmetal/altered-draws", dm_counts.altered_draws);
            CloseHandle(fh);
        }
    }
    return TRUE;
}
