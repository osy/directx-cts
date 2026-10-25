# Backend layers

The shims never change what a test measures ([shims.md](shims.md)). What a particular
host D3D implementation gets wrong — a create it aborts the process on instead of failing, a
format it uploads incorrectly, a device it never frees — is a different kind of
workaround: it changes results, it is specific to that implementation, and the same
implementation can sit in the process (D3DMetal in a CrossOver bottle) or behind a
guest driver that forwards to it (the Neptune VM). So those live in a **backend
compatibility layer**: one DLL per implementation, `dxcts_backend_<name>.dll`, staged
beside the shims on every target and applied only when a run names it:

    ./dxcts run --api d3d11 --per-group --backend d3dmetal --out report.json

The d3d11 and d3d12 shims load it from beside themselves and route every device
creation through it (`src/common/backend.h`); its wrapper sits under the shims' own.
The report records the name under `selection.backend`, and its `workarounds` array
lists what the layer does and whether each part was on. Running the same layer on
both arms is what makes a CrossOver run and a Neptune run comparable: a difference
between them is then the driver's, not a difference in what was worked around.
Implementations with no layer - DXMT, whether in CrossOver or behind the Neptune VM, and
WARP - run without `--backend`.

A layer exports four functions (`src/common/backend.h`, and the layer's `.def`):
`dxcts_backend_d3d11_create_device`, `dxcts_backend_d3d11_create_device_and_swapchain`
and `dxcts_backend_d3d12_create_device`, each handed the real entry point to create
through (so a layer can also serve a creation itself, as the D3DMetal layer does with a
parked device), and `dxcts_backend_workaround`, which enumerates what the layer does for
the report. `DXCTS_BACKEND` must be a plain lower-case identifier, so it can only name
a DLL beside the shims.

## The D3DMetal layer

**`d3dmetal`** (`src/shims/backend_d3dmetal/`) carries the workarounds for Apple's
D3DMetal, for its D3D11 and D3D12 devices. It is selected the same way on both arms that
use D3DMetal: CrossOver, where D3DMetal is in the test process, and the Neptune VM,
where a Windows guest driver forwards to it on the host.

### Knobs

Each workaround is a knob, `DXCTS_D3DMETAL_<WHAT>`, on whenever the layer is selected
and off with `=0` (`--no-patches` turns them all off). The report's `workarounds` array
lists each with whether it was on, and each group's `altered_draws` counts the draws a
guard dropped or changed:

| Knob | What it does |
|---|---|
| `MSAA_QUALITY_LEVELS` | answer S_OK with zero levels for a count or format Metal cannot carry, and where the host answers E_INVALIDARG |
| `FEATURE_SUPPORT` | answer "not supported" where CheckFeatureSupport returns E_INVALIDARG |
| `FORMAT_SUPPORT` | clear the capability bits a format class cannot have (the host reports 0xffffffff for every format) |
| `TEXTURE_GUARD` | refuse a texture, or a D3D12 allocation-info query, of a format, depth-stencil bind or sample count Metal cannot make (Metal's validation aborts the process otherwise) |
| `CREATE_GUARDS` | refuse render-target views of buffers, 32-element or unnamed-element input layouts, stream-output declarations with gaps or over 53 entries; drop render-target barriers of D3D12 buffers and indexed draws recorded without an index buffer |
| `DRAW_GUARDS` | never bind a pass-through geometry shader (in D3D12, refuse a pipeline state with any geometry shader beside tessellation), drop a tessellated draw over D3DMetal's per-patch threadgroup budget, turn alpha-to-coverage off on a target Metal cannot blend, and skip a depth MIN or MAX resolve where Metal would abort — the one class that alters results, counted per group as `altered_draws` |
| `FETCH_BOUNDS` | issue a draw whose vertex or index fetches reach past a bound buffer on a zero-padded copy of it, which is what D3D defines the reads as |
| `RGB32` | create R32G32B32 textures as their RGBA32 twin and repack every CPU-visible byte path (Metal has no 96-bit format; every D3DMetal upload of one leaves it zeroed) |
| `DEPTH_STENCIL_UPLOAD` | rewrite the stencil plane after every CPU write into a depth-stencil texture (D3DMetal writes the depth plane only) |
| `BC_COPY` | serve block-compressed to uncompressed copies through a staging readback, and overlapping same-resource 3D copies through a temporary |
| `DEVICE_REUSE` | park a released D3D11 device and serve the next create with the same arguments from it (D3DMetal never frees one: 256 MiB per create/release cycle, and some HLK binaries create a device per case) |
| `DRAIN_DELETES` | flush every 256 texture creates (the host defers destroying released resources until a flush) |
| `ENQUEUE_SET_EVENT` | implement IDXGIDevice2::EnqueueSetEvent with a fence where the host answers E_NOTIMPL |
| `MINPREC_SV_POSITION` | refuse a pixel shader declaring minimum precision on SV_Position (the host deadlocks drawing with one) |
| `D3D12_FEATURE_SUPPORT` | answer "not supported" where D3D12 CheckFeatureSupport returns E_INVALIDARG |

The layer's own capability table (`formats.c`) only ever narrows: a texture it refuses
is one Metal cannot make on any Apple GPU, and a query it answers "no" to is one the
host would have aborted on. What the host merely might not support is left as the host
answered.

## MTL_ERROR_MODE

D3DMetal reaches `abort()` whenever Metal rejects a descriptor:
`WGF11ResourceFormats\Formats` aborts on `MTLTextureDescriptor has invalid pixelFormat
(0)`, and the shims' crash handling ([running.md](running.md#crashes)) turns that into a
prompt crash rather than a hang.

**It only works when the aborting thread is one of Wine's.** Wine's `abrt_handler`
calls `setup_exception`, which reaches the thread's syscall frame through its TEB; on
a thread Wine never created - a libdispatch worker, which is where D3DMetal compiles
pipelines - that faults, re-enters `segv_handler`, and no Windows-visible exception is
ever raised. `WGF11Precision`'s eight `ShaderInstructions` groups were of that kind
(`Mesh Render Pipeline Descriptor Validation / meshFunction must not be nil`, from
`CreateRenderPipelineStateWithMeshDescriptor` on a concurrent queue). A sample of the
wedged process shows it: two nested `_sigtramp` frames under `abort`, and the test
thread parked in `NtWaitForMultipleObjects` waiting for a pipeline that will never be
built.

**So the runner stops Metal aborting in the first place**, with `MTL_ERROR_MODE=nslog`
in the test process's environment. Metal then reports the same validation failure and
returns instead of calling `abort()`: the check still runs and still names the problem
on stderr, so nothing is hidden - the process simply survives it. This is a property
of the host rather than of anything D3D11 was asked to do, so it belongs in the run
and not in a shim, and it is the right fix for the whole family rather than for one
group at a time. Measured before the D3DMetal layer existed, so without any of its
guards:

| group | without it | with it |
|---|---|---|
| `ResourceAccess\ResourceAccess` | 1774 cases, crash | **91629 cases** |
| `ResourceAccess\CopyMultisample` | wedged, no verdict | **1536 cases in 6.4s** |
| `ResourceAccess\PlanarTextures` | 1 case, abort | **2304 cases** |
| `Multisample\Coverage` | 889 cases, wedge | **8880 cases, 1808 passing** |

`WGF11ResourceAccess` as a whole goes from 102 cases to **101815**.

Metal reads it from the **Unix** environment and a PE cannot put anything there:
`exec_wineloader` execve's each child with the launching Unix process's environ, so it
has to be set before Wine starts. That is what the generated `dxcts` launcher is for -
leaving it to whoever invokes the suite means a forgotten variable looks like a driver
hang. Wine imports the Unix environment into the Win32 one at startup, so the runner
can still tell whether it was set: it says so on its progress stream when it is missing,
and records what it saw in the report's `workarounds` either way.

Where D3DMetal is not in the test process - behind a guest driver, on the Neptune VM -
the host's Metal is not in this environment at all, and a validation failure takes the
host-side worker down instead; that is what most of the layer's guards are for. On
CrossOver they still matter where `MTL_ERROR_MODE` is not enough: a nil `meshFunction`
leaves Metal returning a pipeline that cannot be used, and `WGF11Precision` then stalls
after three cases rather than aborting ([limitations.md](limitations.md)); and a
pipeline Metal refused still leaves the case's result unproven, as below.

### A validation failure that no longer aborts is still a failure

`MTL_ERROR_MODE=nslog` is what lets a group finish instead of wedging, but the call
the host rejected still failed, and the test carries on with whatever it got back. A
case whose expected result is zeroes can then **pass** on a resource the host refused
to create - a silently wrong result in place of a loud one.

So the runner counts them and says so:

* `host_validation_errors` per group and in the totals;
* `pass_in_groups_with_host_errors` in the totals - passes recorded in a group where
  the host rejected something, which have not been shown to mean anything.

Without a backend layer they are common: `WGF11ResourceAccess` at one case per group
once survived 1,794 of them, with 14 of its 18 passes in groups carrying one. The
D3DMetal layer refuses most of those combinations before they reach the host; with it,
the same sample on CrossOver (2026-10-25) has 3, and 377 passes.

They are counted out of the runner's **own** stderr, which the `dxcts` launcher points
at `dxcts-out/host-stderr.log`. Metal and AGX write to the process's Unix fd 2 and
every test process inherits that from the runner, so redirecting the child's Windows
stderr does not move them - the per-group `.err` files stay empty however many there
were. That is the same Win32/Unix split that `MTL_ERROR_MODE` itself has to live with.
