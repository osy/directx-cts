# Known limitations

Measured against D3DMetal under Wine unless said otherwise.

* **`WGF11Filter` and `WGF11SharedResources` enumerate their groups only if the device
  answers `{4BCC5537-3A9C-4573-8D65-8F5B7B15FEF4}`.** Eight of the kit's binaries query
  that interface on the device and no Wine or D3DMetal module has it.
  `QueryDeviceProperties` stores the result of a method on it at `+0x4fb8`, and the
  branch of `InitTestGroups` taken when the driver reports WDDM 1.3 or newer tests that
  field before registering a group, so a failed query stops every group being added —
  `InitTestGroups` gives up with only "Failure setting up test groups" and no log even
  under `-LogAll`.

  The d3d11 shim answers the query with an object whose methods report zero, which is
  what a device that has the interface and nothing to say through it looks like. Both
  binaries then enumerate on the driver version the host reports, with no version
  invented: `WGF11Filter` lists 38 groups (692 cases at one case per group on
  CrossOver, 2026-10-25) where without the answer it lists none, and
  `WGF11SharedResources` 20.

* **A pass-through geometry shader bound together with tessellation aborts the
  process.** D3DMetal folds a geometry shader that only copies its input vertices into
  the stages around it, but with a hull and domain shader bound it still draws through
  a Metal mesh pipeline, and that pipeline then has no mesh function: Metal's
  validation calls `abort()`, on one of D3DMetal's own libdispatch workers, where Wine
  cannot turn a signal into an exception, so the process wedges instead of dying.
  `WGF11Precision`'s eight `LowPrecision\ShaderInstructions` groups and
  `D3DConf_11_Core`'s `RTVPFromVSDS` end there. Any other geometry shader under
  tessellation draws.

  The d3dmetal layer recognises a pass-through shader from its bytecode when it is
  created (point, line or triangle input; each declared output component copied from
  the same component of the same input register, one emit per input vertex) and never
  binds it, which is what D3DMetal makes of it outside tessellation anyway. In D3D12
  the combination is fixed when the pipeline state is created, so there a pipeline
  state with any geometry shader beside a hull or domain shader is refused.

  Measured on `WGF11Precision`, every group, whole binary: **5 cases in 965s with
  eight groups timing out becomes 3640 cases in 144s, all executed, 1165 passing**,
  and the failures are `Result not within expected ranges` rather than setup errors.

  The one-case-per-group sweep is the right shape for catching regressions and the
  wrong one for measuring gains: it cannot see a fix that lets a group run further,
  and it hides a crash that happens deeper in. `ResourceFormats\Formats` gaining 720
  cases shows up in it as nothing at all. Run the group whole to measure a fix; run
  the sweep to check it broke nothing.

* **Alpha to coverage on an integer render target aborts the process.** D3D11 takes
  the combination - `WGF11Multisample\Coverage` enumerates
  `RT_resource_format=R10G10B10A2_UINT` with `A2CEnable` as a test parameter - but
  Metal refuses the pipeline (`alphaToCoverageEnabled requires a float/unorm pixel
  format`). The d3dmetal layer draws to a target Metal cannot blend with
  alpha-to-coverage off, counting each such draw in the group's `altered_draws`. Under
  `MTL_ERROR_MODE` alone the verdicts are the same - all 8880 cases agree, 1808
  passing - but every pass then sits in a group with 42 host validation errors; the
  layer is what keeps them out, and on a guest driver, where Metal does abort, what
  keeps the process alive.

* **A depth-stencil format used as a buffer aborts the process.** D3D11 defines none
  of the `R32G8X24` or `R24G8` families, nor `D32_FLOAT` or `D16_UNORM`, as a buffer
  or input-assembler format, but the host answers every capability for every format,
  so `WGF11ResourceFormats\Formats` walked into creating a typed buffer of one.
  D3DMetal has no Metal pixel format for that and builds a descriptor with
  `pixelFormat 0`, which fails validation and aborts.

  Reporting those combinations unsupported takes the group from **145 cases to 865**,
  and from 9 formats reached to 15.

* **The host does not implement `CheckFormatSupport`.** Measured with a probe: it
  answers `0xffffffff` for every DXGI format, so every format claims every capability.
  The d3dmetal layer clears the bits D3D11 fixes for block-compressed, typeless and
  depth-family formats — none of the three is a render target, a buffer or an
  input-assembler format, and a typeless one cannot be read by a shader either — so the
  test skips the group instead of asking for a combination that wedges the host in
  pipeline compilation. The depth-family formats are the worst case:
  `Blend\Format\R32_FLOAT_X8X24_TYPELESS` spun at ~178% CPU with no further events, and
  becomes a 5 s `Skipped` case. Everything the host merely might not support is left as
  it answered. `CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT)` is the same query by
  another name and gets the same correction.
* **Some groups are slow rather than hung.** At one case per group on CrossOver,
  `WGF11Clipping`'s `Line\ClipDist`, `Line\CullDist`, `Triangle\ClipDist`,
  `Triangle\CullDist` and `Triangle\ZClip` each take about four minutes, and the whole
  `ZClip` group (24,256 cases) far longer; WARP itself needs over 900 s for some
  `RenderTargets` groups. That is why the default timeout is two hours; `--slow`
  names them.
* **`--sample` is a percentage, not a fraction, and rounds up to at least one case per
  group.** `--sample 0.002` is therefore "one case per group", which exercises every
  group in the suite. The space is large: `WGF11Shader5x` enumerates 336,683 cases and
  `WGF11Blend` 302,919, so a full run is a very long one.
* **`--finish-ms`/`--query-ms` default to 1000 ms** against the framework's 10,000,
  bounding waits a host implementation never satisfies; raise them when the wait itself
  is under test.
* **TAEF buffers its console output until the process exits**, so a TAEF process
  killed at its timeout yields no case records at all - only the fact that it timed
  out. `--per-group` bounds that to one class.
* **`D3DConf_12_Video` can bugcheck a VMware guest's display driver** (`0x1B
  PFN_SHARE_COUNT`) when it runs after the other TAEF binaries, taking the machine down
  mid-sweep. It runs to completion on its own (14,648 cases on WARP, no timed-out
  group), so on such a machine run it apart from the rest.
* **Four groups run no cases, for two distinct reasons.**
  `D3DConf_12_1_AllocationInfo`, `D3DConf_12_1_PSOdedupe` and
  `D3DConf_12_4_ContentProtection` consume an **ETW trace** to check their results:
  `EtwTraceHelper::GatherEvents` fails at `Consumer.OpenTrace()` with `E_ACCESSDENIED`
  and the log file is never created, so the class setup fails before any case. They run
  no cases under WARP either — where the adapter is ACG-compatible and the class calls
  `SetProcessMitigationPolicy(ProcessDynamicCodePolicy)` instead of taking the ETW path
  at all — so this belongs to OS facilities rather than to D3D.
  `D3DConf_12_Raytracing_RayQuery` takes a null dereference inside D3DMetal, at
  `CreateStateObject`: the host reports a raytracing tier of 1, which is not a value the
  enumeration has (NOT_SUPPORTED is 0 and TIER_1_0 is 10), and then faults when a test
  believes it.
* **`iFlip` runs only because the dxgi shim answers a DWM-private query.** It does
  `pOutput->QueryInterface(&pDWMOutput)` for `IDXGIOutputDWM`, which Wine does not
  implement, and calls `GetMultiplaneOverlayCaps` on the null result without checking
  (the faulting register holds 0x80004002, E_NOINTERFACE), so both its groups die
  before a case. The shim answers that query with an object whose methods report
  failure, which turns the crash into 84 cases that run and fail on their own terms;
  iFlip runs the same 84 under WARP, where the real interface exists.
* **Not reached by the shims**: `DirectML.dll`, which the three DirectML/NPU binaries
  need, and the optional TAEF test modes, none of which is a test
  ([shims.md](shims.md#static-load-check)).
