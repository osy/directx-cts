# directx-cts

A free-standing conformance/regression suite for Direct3D implementations, built from
Microsoft's Windows Hardware Lab Kit (HLK) test content.

The HLK holds by far the largest body of D3D11/D3D12/DXGI conformance tests in
existence, but ships locked to a Windows-only controller and job scheduler. The test
binaries themselves are self-contained: they carry their own case enumeration and
verdict logic, and the controller merely shells out to them. This project supplies the
three things that are actually missing — a reproducible way to fetch the kit, a
compatibility shim so the binaries load under unmodified Wine, and a runner that drives
them and emits one machine-readable report.

Targets Wine and Windows, on x86, x64 and arm64.

## Layout

    kit.toml        pinned HLK release and the installer payloads to fetch
    scripts/        kit acquisition (fetch, extract, stage)
    suites/         test catalog: binary -> harness family, dependencies, arguments
    src/runner/     dxcts.exe
    src/shims/      compatibility and check-bypass DLLs, and the backend layers
    src/common/     COM wrapping, workaround knobs, backend hand-off, event stream, JSON, shared DllMain
    tools/          PE inspection, generated-input refresh, static load check, report comparison
    cmake/          toolchain files

`kit/`, `staging/` and `build/` are generated and never committed. **No HLK
content belongs in this repository.**

## Getting the kit

Requires `7z` and `msitools` (macOS: `brew install p7zip msitools`).

    ./scripts/fetch_kit.py --plan     # show what would be downloaded
    ./scripts/fetch_kit.py            # ~1.7 GB into kit/Installers
    ./scripts/extract_kit.py          # -> kit/WindowsKits10

Fetching needs no Windows and runs no installer. The bootstrapper behind the pinned
fwlink is a WiX Burn bundle; its manifest gives every payload's size and SHA-1, and each
MSI's external cabs come from its own Media table, so only the required bytes are
pulled. Both are verified on download.

Extraction ends with a closure check (`[verify]` in `kit.toml`): if a future kit moves a
file between packages, it fails naming the missing paths instead of producing a tree
that breaks at run time.

To adopt a newer HLK, change `fwlink` and `version` in `kit.toml`. A version mismatch
against the live manifest is reported rather than silently accepted.

## Building the shims

Needs a Windows cross-compiler. mingw-w64 covers x86 and x64
(`brew install mingw-w64`); arm64 needs [llvm-mingw] or `zig` (`brew install zig`),
either of which also covers the other two.

    cmake -B build/x64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake -DDXCTS_ARCH=x64
    cmake --build build/x64

On Windows with MSVC, configure without the toolchain file.

[llvm-mingw]: https://github.com/mstorsjo/llvm-mingw/releases

## Staging

    ./scripts/stage.py --arch x64                      # -> staging/x64, for Wine
    ./scripts/stage.py --arch arm64 --target windows   # -> staging/arm64

Produces one flat directory holding the test binaries, their support DLLs and data,
TAEF's MinTe runtime, and the shims. Copy it into a Wine bottle or a Windows VM and run
from there. `dxcts-staging.json` records the kit version, the active shims and every
binary that was rewritten.

`--target windows` stages only the shims marked as needed on every target — the WTTLog
proxy, which is the results channel rather than a compatibility fix, the three whose
behaviour a WARP run depends on, and the d3d11 shim, which is where a backend layer is
applied. The rest are omitted, because Windows provides those modules and a shim in the
application directory would shadow the real one. Every workaround inside a shim that
*is* staged there is off by default; see [Knobs](#knobs). The backend layers are staged
on every target too, and do nothing unless a run names one (see
[Backend layers](#backend-layers)).

The kit is copied, never modified.

## Shims

Wine is never modified. Most shims are ordinary DLLs staged beside the tests, where
the application directory wins the search. Two cases cannot be reached that way, and
those binaries get a **length-preserving import-name rewrite** applied to the staging
copy:

* apiset names (`api-ms-win-*`) are resolved by Wine's `apisetschema.dll` before the
  file search runs, so a DLL of that name beside the executable is never consulted;
* `d3d11`, `d3d12`, `dxgi`, `dcomp`, `oleaut32`, `powrprof` and `rpcrt4` are Wine
  builtins, which win over a native DLL in the application directory.

Some modules are also resolved with `LoadLibrary` on a string literal rather than
through the import table, so the literal is rewritten too. That search covers both
encodings and any spelling: DxgTaef's device cache loads `L"DXGI.dll"` — wide and
upper-case — and `GetProcAddress`es `CreateDXGIFactory2` out of it, which an ASCII
lower-case search does not see, and the shim is then staged, imported and never
called.

Rewrites are applied only where Wine would actually fail to bind the import, or
where its implementation resolves but misbehaves. `stage.py` reports how many
binaries each one touched, and `dxcts-staging.json` records them by name.

| Shim | Reached by | Why |
|---|---|---|
| `DEVOBJ.dll` | app directory | absent from Wine; forwards to setupapi |
| `dxcts_d3dkmt_compat_shim_v1.dll` | import rewrite | D3DKMTSubmitCommand/SubmitCommandToHwQueue/EnumAdapters3 live in win32u, not the gdi32 the apiset resolves to; and Wine's OpenAdapterFromDeviceName and QueryAdapterInfo reject the adapter, emptying every -AdapterType subset |
| `dxgdi.dll` | import rewrite | the DXGI and D3D9-era binaries import `D3DKMTOpenAdapterFromDeviceName`/`QueryAdapterInfo`/`CloseAdapter` from gdi32 directly rather than through the apiset, so the d3dkmt shim never sees them and Wine's answers empty every `-AdapterType` subset ("adapter subset of size (0)"); a generated forwarder (`tools/gen_gdi32_def.py`) that sends those to the d3dkmt shim and everything else back to gdi32 |
| `dxc11.dll` | import rewrite | CreateDirect3D11DeviceFromDXGIDevice has no Wine equivalent and five DXGI tests import it; the device needs wrapping to answer the interface eight binaries probe it for; and device creation is where a backend layer is applied |
| `dxc12.dll` | import rewrite | D3D12GetInterface is in d3d12core, not d3d12; PIX hooks stubbed; WARP substituted for a null adapter; device creation is where a backend layer is applied |
| `dxcg.dll` | import rewrite | adapter enumeration has to be answerable per binary: a software adapter for DxgTaef's scope fixture, WARP only for the reference arm, an IDXGIOutputDWM stub for iFlip |
| `dxcdc.dll` | import rewrite | D3DConf_11_Core statically imports dcomp ordinal 2005 (CreatePresentationFactory on Windows); Wine's dcomp stops at 1042, so the whole 39-method D3D11.3 binary fails to load |
| `dxcts_power_setting_compat_shim.dll` | import rewrite | DXGIHybridPresent reaches the power accessors through this apiset, which the schema resolves before any file search, so the powrprof shim cannot be reached by its own name |
| `dxctspwr.dll` | import rewrite | Wine implements none of the Power*ValueIndex accessors DXGIHybridPresent reads its GPU preference through, and the two it does export return success without writing the caller's out-parameter |
| `dxcts_crt_private_compat_shim.dll` | import rewrite | Wine's ucrtbase lacks __C_specific_handler_noexcept, which d3ddriververifier, d3d11_3sdklayers and dxgidebug all bind; it lives in vcruntime140 |
| `dxctsole.dll` | import rewrite | TE.Model binds the *_User*64 marshalling helpers; Wine exports only the unsuffixed names, which are the 64-bit implementations on x64 |
| `WTTLog.dll` | renamed original | records the per-case event stream; both HLK framework generations log through CWTTLogger, so it is the one results channel covering them, and the only way to tell a case that never ran from one that failed |
| `dxrpc4.dll` | import rewrite | TE.Common/TE.Model import Conduit.Broker statically, and it binds six RPC entry points Wine lacks; stubbed because only remote execution calls them |

Check what would fail to load before running anything:

    ./tools/check_wine.py --summary --app-dir staging/x64 staging/x64

This resolves every import against Wine's builtin exports, apiset redirection
included. It is static: a clean result means the loader will accept the image, not
that the test passes. Set `WINE_DLL_DIR` if the Wine installation is somewhere it does
not look.

### Knobs

Every workaround for a defect is one switch, so what it is worth can be measured rather
than argued about — see `src/common/knob.h`. `DXCTS_<SHIM>_<WHAT>=0` turns one off,
`=1` forces it on, and `dxcts run --no-patches` (or `DXCTS_NO_PATCHES=1`) turns every
one off at once; the report records that it was set.

The shims beside the tests only make a binary load and start: their knobs are for
things the *platform* lacks, so their defaults differ between Wine and Windows.

| Knob | Wine | Windows | What it does |
|---|---|---|---|
| `DXCTS_D3D11_DEVICE_PROBE_IFACE` | on | off | answer the undocumented device interface eight binaries query |
| `DXCTS_D3D11_LOAD_VERIFIER` | on | off | load D3DDriverVerifier.dll before the device exists |
| `DXCTS_D3DKMT_ADAPTER_NAME_FALLBACK` | on | off | fall back to the primary GDI display name |
| `DXCTS_D3DKMT_ADAPTER_INFO_FALLBACK` | on | off | answer the adapter-type query the classifier needs |

Everything the host D3D *implementation* gets wrong lives in a backend layer instead
(below), whose knobs are on whenever the layer is selected, on either platform.

Workarounds with no knob are the ones nothing launches without on either platform:
an unbound static import, the results channel, and suppressing the JIT-debugger handoff
(see `src/common/shim.h`).  `MTL_ERROR_MODE` is not a knob either, for the opposite
reason: it is set outside the process by the launcher, and the report records what the
run actually saw rather than what it asked for.

Every knob that works by wrapping a COM object needs a `this`-swapping thunk; x86, x64
and arm64 each have one (`src/common/comwrap.c`), and they are compiled in rather than
generated at run time: the HLK binaries opt into Windows' dynamic-code prohibition
(`SetProcessMitigationPolicy`), a no-op under Wine but one that makes an executable
allocation fail in a test process on Windows, where a generated thunk would silently
have left every wrapper unbuilt. The standalone stub object some knobs hand out has a
narrower limit: on x86 a COM method pops its own arguments, and a stub cannot know how
many an undocumented method takes, so there it declines the interface instead. See
`src/common/comwrap.h`.

Separate from the knobs, three things are *requests* the runner makes per binary or per
run from the catalog rather than platform judgements: `DXCTS_SOFTWARE_ADAPTER`,
`DXCTS_DWM_OUTPUT` and `DXCTS_WARP_ONLY`. A request says what a binary needs in order to
be testable at all; a knob says whether a defect is worked around. A knob never vetoes a
request.

### Backend layers

The shims above never change what a test measures. What a particular host D3D
implementation gets wrong — a create it aborts the process on instead of failing, a
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

**`d3dmetal`** (`src/shims/backend_d3dmetal/`) carries the workarounds for Apple's
D3DMetal. Each is a knob, `DXCTS_D3DMETAL_<WHAT>`, on whenever the layer is selected:

| Knob | What it does |
|---|---|
| `MSAA_QUALITY_LEVELS` | answer S_OK with zero levels for a count or format Metal cannot carry, and where the host answers E_INVALIDARG |
| `FEATURE_SUPPORT` | answer "not supported" where CheckFeatureSupport returns E_INVALIDARG |
| `FORMAT_SUPPORT` | clear the capability bits a format class cannot have (the host reports 0xffffffff for every format) |
| `TEXTURE_GUARD` | refuse a texture, or a D3D12 allocation-info query, of a format, depth-stencil bind or sample count Metal cannot make (Metal's validation aborts the process otherwise) |
| `CREATE_GUARDS` | refuse render-target views of buffers, 32-element or unnamed-element input layouts, stream-output declarations with gaps or over 53 entries; drop render-target barriers of D3D12 buffers and indexed draws recorded without an index buffer |
| `DRAW_GUARDS` | drop a draw binding a geometry shader with tessellation or a tessellated draw over D3DMetal's per-patch threadgroup budget, never bind a pass-through geometry shader, and turn alpha-to-coverage off on a target Metal cannot blend — the one class that alters results, counted per group as `altered_draws` |
| `FETCH_BOUNDS` | issue a draw whose vertex or index fetches reach past a bound buffer on a zero-padded copy of it, which is what D3D defines the reads as |
| `RGB32` | create R32G32B32 textures as their RGBA32 twin and repack every CPU-visible byte path (Metal has no 96-bit format; every D3DMetal upload of one leaves it zeroed) |
| `DEPTH_STENCIL_UPLOAD` | rewrite the stencil plane after every CPU write into a depth-stencil texture (D3DMetal writes the depth plane only) |
| `BC_COPY` | serve block-compressed to uncompressed copies through a staging readback, and overlapping same-resource 3D copies through a temporary |
| `DEVICE_REUSE` | park a released D3D11 device and serve the next create with the same arguments from it (D3DMetal never frees one: 256 MiB per create/release cycle, and the HLK creates a device per case) |
| `DRAIN_DELETES` | flush every 256 texture creates (the host defers destroying released resources until a flush) |
| `PIN_CRYPTO` | keep bcrypt and crypt32 loaded (the host loads and frees both around every shader hash; under Wine each cycle leaks dyld state) |
| `ENQUEUE_SET_EVENT` | implement IDXGIDevice2::EnqueueSetEvent with a fence where the host answers E_NOTIMPL |
| `MINPREC_SV_POSITION` | refuse a pixel shader declaring minimum precision on SV_Position (the host deadlocks drawing with one) |
| `D3D12_FEATURE_SUPPORT` | answer "not supported" where D3D12 CheckFeatureSupport returns E_INVALIDARG |

The layer's own capability table (`formats.c`) only ever narrows: a texture it refuses
is one Metal cannot make on any Apple GPU, and a query it answers "no" to is one the
host would have aborted on. What the host merely might not support is left as the host
answered.

## Running

`dxcts.exe` sits in the staging directory and is run from there, inside the bottle or
the VM. **On Wine, run it through the `dxcts` launcher beside it** rather than invoking
`dxcts.exe` yourself: staging generates it, and it supplies the host environment the
run needs before Wine starts - `MTL_ERROR_MODE`, which a PE cannot set for itself and
which is what keeps a Metal validation failure from wedging the group (below). It derives the bottle name and the `C:` path from where it was copied to, so
there is nothing to configure:

    ./dxcts run --api d3d11 --per-group --out report.json

`WINE` overrides which wine it uses; outside a CrossOver bottle it falls back to
`wine` on `PATH` with whatever `WINEPREFIX` is set. The report's `workarounds` records
what it saw, so a run made some other way still explains itself.

    dxcts list  --api d3d11
    dxcts run   --api d3d11 --sample 0.002 --out report.json
    dxcts run   --binary WGF11Blend.exe --group 'Blend\Format\R32G32B32A32_FLOAT'
    dxcts probe
    dxcts run   --api d3d11 --per-group --timeout 60 --out report.json

Selection is by API, harness family, binary and group, plus deterministic sampling
(`--sample 0.002 --seed 1`). Sampling and group selection are spellings the WGF
framework has and TAEF and gtest do not, so a binary that could not be given one is
recorded with `selectors_ignored` rather than counted as if it had been.

Progress goes to stdout, and so does the report when there is no `--out`, so in that
case progress is suppressed and the report is the only thing written there. `--slow
SECONDS` (default 60) names a group that runs long without failing it; the count reaches
the report as `slow_groups`. Tests are spawned rather than hosted, so a crashing or
wedged one costs only its own result. `--timeout` bounds each spawned process — each
group under `--per-group`, each binary otherwise — and given explicitly it also
overrides the per-binary value the catalog carries.

`--11on12` runs the D3D11 WGF binaries through D3D11On12, so the D3D12 implementation is
what is under test; that is how the kit's own D3D12 job list runs them, and the report's
`selection.d3d11on12` records it.

Wine 10 starts every process with a limited administrator token, and some tests refuse
one ("This test must be run elevated." - DXGIMultiPresent; the ETW listeners behind the
present tests). When its own token is limited the runner starts every test under the
token's linked full token instead, handle inheritance and the job intact; Windows denies
that to an unprivileged caller and the tests then run under whatever token the runner
has. The report's `target.elevation` says which: `elevated`, `linked-token` or `limited`.

`--per-group` runs each test group as its own process. It costs a process launch per
group, but a group that wedges the host then costs only its own timeout instead of
everything after it in that binary. Measured across the 28 WGF D3D11 binaries against
D3DMetal, one case per group (`--sample 0.002 --timeout 60`): **2607 cases, all
executed, 1119 passing**. Whole-binary execution yields far fewer, because the first
group that wedges the host ends the binary: `WGF11Shader5x` gives 1 case that way
against 367 per-group.

Under Wine a test that crashes does not die: it hangs, and the runner can only see that
as a timeout, because Wine's `UnhandledExceptionFilter` hands the process to
`winedbg --auto` and waits on a debugger that never signals. The shims therefore set
`SEM_NOGPFAULTERRORBOX` at load, which short-circuits that filter so ntdll terminates
the process instead — see `src/common/shim.h` for the mechanism. It is in the run rather
than in the bottle, and it means the same thing on Windows, where it suppresses the WER
dialog that would stall an unattended run identically.

This matters because D3DMetal reaches `abort()` whenever Metal rejects a descriptor:
`WGF11ResourceFormats\Formats` aborts on `MTLTextureDescriptor has invalid pixelFormat
(0)`, and is reported as a crash in six seconds rather than as a 120 s hang.

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
group at a time:

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

Look here first when a group wedges. Working around what a particular abort *does* -
dropping the draw, substituting state - is a much worse answer: it is pinned to the
backend's internals, and the environment gives the same numbers with no code at all.

`MTL_ERROR_MODE` is not enough on its own for a nil `meshFunction`: Metal returns a
pipeline that cannot be used, and `WGF11Precision` then stalls after three cases
rather than aborting. That one still needs the shim below.

### Exclusions

What still wedges, crashes or otherwise breaks a run on a given implementation after
everything here has been fixed is kept out of it by an **exclusion list**, one per
implementation, in `suites/exclusions/<name>.jsonl`. With it, one command runs every
test that can run and gives the same answer twice:

    dxcts run --per-group --src WARP --exclusion warp --out warp.json
    dxcts run --per-group --backend d3dmetal --exclusion d3dmetal --out neptune.json

A bare name is a list staged beside `dxcts` (staging copies `suites/` whole); anything
with a separator or extension is a path. Each line is one flat JSON object, like the
catalog:

    {"binary":"WGF11Foo.exe","group":"Foo\\Bar","cases":"12-15,40","reason":"...","evidence":"..."}

* `binary` alone excludes the binary; `harness` (`wgf`, `wgf9`, `taef`, `gtest`) in
  place of or alongside it names a whole framework family, for the case where one
  reason covers every binary of it. `group` narrows it to a group — and, for WGF, the
  subtree under it, as the framework's own `!Group` does. `cases` narrows it further:
  WGF case numbers (the number the log prints before each `End:`), or the TAEF method /
  gtest test names within the group. `binary` and `group` take `*` and `?`.
* **`reason` is required**, and a line without one is refused, as is any key the runner
  does not know (a misspelt `group` would otherwise widen the entry to the whole
  binary). `evidence` is free text: the run, log or stack that showed it.
* Fix the harness first. A line belongs here only when the fault is the
  implementation's and cannot be survived — a timeout, a wedge, a process death that
  takes the group with it. A case that fails is a result, never an exclusion.

How each family is told: WGF takes `!Group` and `!Group:1-3,7` as positionals and
neither runs nor logs what they name, so the counts are not inflated by skips; TAEF gets
a `/select` query; gtest a negative `--gtest_filter`. Under `--per-group` an excluded
group is simply never spawned. A wgf9 (D3D9-era) binary has no group tree, so only a
whole-binary entry applies to it; anything finer is reported under
`selectors_ignored` as `exclusion` rather than quietly run, and so is a WGF wildcard
group in a whole-binary run, since the framework has no wildcards of its own.

The report says what was left out and why: `selection.exclusion` names the list, each
binary carries `excluded` (the whole binary) or an `exclusions` array (groups not
spawned, case sets skipped, each with its reason and line), and `totals` counts
`excluded_binaries`/`excluded_groups`/`excluded_case_sets`. `stale_exclusions` lists the
entries whose binary ran without them matching anything — a group renamed or gone —
so the list only ever shrinks. (Staleness is judged under `--per-group`, where the
runner sees the group list; a whole-binary run hands entries to the framework blind.)

The catalog's `skip` is different: it marks a binary that cannot run on *any*
implementation (it needs a second adapter, a capture device), and stays there.

### What the report answers

The report separates **a case that ran and failed** from **one that never ran**. That
distinction cannot be recovered from a log file: the HLK framework fails a case for
setup and environment problems using the same verdict it uses for a genuine content
mismatch. It comes instead from the WTTLog proxy, which sees the real StartTest and
EndTest calls, so every case carries `executed` alongside its `result`.

Both HLK framework generations log through `CWTTLogger`, so one proxy covers them.
TAEF is the exception: `Wex.Logger.dll` binds only the flat `WTTLogTrace*` entry
points and never calls StartTest or EndTest, so there is no start/end pair to observe.
Its cases are read back from `TE.exe`'s console output instead — one
`StartGroup:`/`EndGroup:` pair per method — and TAEF's own `Summary: Total=` is
recorded as `harness_reported_cases`, so a binary whose output and case records
disagree is visible rather than believed. TAEF's "Not Run" stays `executed: false`
rather than becoming a verdict. A gtest binary is read from the report it was asked
to write with `--gtest_output=json:` - JSON, or the gtest XML that Dawn's own launcher
writes into that path - and it is split one process per suite (`--gtest_list_tests`,
then `--gtest_filter=Suite.*`), because that launcher runs batches in child processes
and gives up on the whole binary when one dies; under D3DMetal the first device test
does, and 240 suites became 6190 recorded cases instead of none.

TAEF runs `/inproc` under Wine, where its out-of-process host deadlocks: `TE.exe` and
`te.processhost.exe` both sit at 0% CPU after the ncalrpc handshake and no test ever
starts. It is Wine-only because it costs coverage: a class whose metadata needs a fresh
host is refused under `/inproc` ("TAEF would need to start a second test host") and every
method in it is blocked - 107 of `D3DConf_12_Core`'s on WARP.

Liveness for the stall watchdog is any growth in what the test writes - the WTTLog
event stream for the HLK frameworks, the console for TAEF and gtest. A gtest binary
never writes an event, and judged on the stream alone Dawn was reported wedged at 600 s
with its output still growing; a TAEF class longer than the stall bound (Conf_VideoNative
under D3DMetal) was cut the same way.

A binary that reads data from a fixed path outside the staging tree - one the HLK job
deploys before running it - names it in the catalog as `deploy_from`/`deploy_to`, and
the runner copies it there first. `DirectML.Test.ModelTests` reads its models from
`%TEMP%\directml.test.modeltests.0.0.4` and fails every method with "Invalid model path"
otherwise.

## Refreshing generated inputs

These inputs are derived from the kit and regenerated when it changes:

    ./tools/gen_catalog.py kit/WindowsKits10 > suites/graphics.jsonl
    ./tools/gen_wttlog_def.py kit/WindowsKits10
    ./tools/gen_gdi32_def.py kit/WindowsKits10

The catalog classifies each binary by fingerprint rather than by a hardcoded list.
The WTTLog `.def` is taken from the kit's own import tables, because `CWTTLogger`'s
exported names are MSVC-mangled, the mangling differs per architecture, and part of
the surface is imported by ordinal. The gdi32 forwarder's `.def` is every gdi32 import
the kit makes, so a binary that starts importing a new one still loads.

Two tools read the reports back:

    ./tools/parity.py reference.json candidate.json [--binary NAME] [--all] [--reasons]
    ./tools/failure_report.py report.json staging/x64/dxcts-out

`parity.py` compares two runs group by group and lists where the second falls short of
the first - fewer passes, fewer cases enumerated, or a group that timed out or wedged -
which is how a shim change is shown to have cost nothing. `failure_report.py` buckets a
run's failures by group, names each group's dominant reason out of its log, and flags
the reasons that point at something this suite supplies rather than at the
implementation.

## The WARP arm

Windows ships WARP, a complete software rasterizer for both D3D11 and D3D12. Running
the same staging tree against it answers "is this the implementation under test, the
test, or this harness?" without changing anything else:

    dxcts run --api d3d11 --harness wgf --per-group --src WARP --out warp.json

Measured on an ARM64 Windows VM, all 28 WGF D3D11 binaries, one case per group:
**6228 cases, 4919 passing, 1291 skipped, 18 failing, no group timed out and no
binary failed a precondition.** So the harness, the staging, the shims' defaults on
Windows, the WTTLog proxy and the report are all sound, and a timeout seen only under
Wine is a property of that implementation rather than of this suite.

Asking for WARP takes three things, because each layer can bypass the one below:

* `-Src:WARP` for the WGF binaries, which is the framework's own device selection;
* hiding every adapter but WARP from `EnumAdapters1` and from
  `EnumAdapterByGpuPreference`, since TAEF has no equivalent switch and DxgTaef takes
  the first adapter DXGI reports; and
* substituting the WARP adapter inside `D3D12CreateDevice`, because the D3D12 suites
  pass a null adapter and the runtime then takes the default one without enumerating
  anything at all.

`--src WARP` does all three - but the hiding only for the TAEF and gtest binaries, which
have no way to ask for a device themselves. The WGF binaries get `-Src:WARP` and keep
the enumeration intact: their DXGI present tests look up the adapter that owns the
monitor's output, and WARP has no outputs, so with everything else hidden every one of
them blocks before its first case ("adapter with no outputs"). It matters: on a machine
with a GPU, missing any one of them leaves the suites running on that GPU's driver
instead.

Two things to know about running on Windows:

* **Tests must run in the interactive console session.** Over SSH they land on a
  service window station, and a WGF binary hangs after creating its device rather
  than failing. Drive them with `schtasks /it` pointed at a `.cmd` wrapper — not at
  the command directly, since `schtasks /tr` mangles embedded quotes and the task
  then silently does nothing.
* **The WTTLog proxy is staged there too**, because it is the results channel rather
  than a compatibility shim. Without it a run reports no cases at all while the
  framework's own log shows the tests passing.

### What the arm settled

Running both arms and comparing group by group attributes every timeout mechanically,
rather than by argument. Across the D3D11 WGF suite and the TAEF suites:

| | Wine/D3DMetal | VM/WARP |
|---|---|---|
| D3D11 WGF: cases / pass / fail | 3642 / 1283 / 940 | 6228 / 4919 / 18 |
| D3D11 WGF: timed-out groups | 13 | 0 |
| D3D12 core: cases / pass | 1712 / 46 | 2071 / 1703 |
| D3D12 core: failed setup fixtures | 0 | 0 |

* **20 groups time out under Wine and not under WARP** — 13 in D3D11, 7 in the TAEF
  suites. Those are defects of the implementation under test.
* **4 groups time out under both** (`D3DConf_12_0_MeshShaders#0#0` through `#1#1`) —
  but for different reasons, which only a long run distinguishes. Given 1200 s under
  WARP the group completes cases steadily, creating pipeline states and signalling
  fences throughout: it is slow, and the catalog now gives that binary the time it
  needs. Given 900 s under Wine it starts one case and never finishes it: there it is
  stuck, like the other 20.
* **4 of the 11 zero-case groups are zero under WARP too**, so they have no applicable
  cases rather than losing them; the rest crash inside the test binary under Wine.

### Debugging a timeout

On Windows, attach `cdb` and read the stack — it is the fastest way to tell a spin from
a wait, and it names the call:

    cdb -pn TE.exe -c "~*k 30;q"

Run it from a `.cmd` in the console session (see above), giving the test time to reach
the hang first. That is how `D3DConf_12_0_MeshShaders` was settled: its test thread sits
in `MeshShadersBase::FlushAndFinish` under `D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU`,
blocked on a GPU fence rather than spinning, so WARP is slow rather than stuck.

Under Wine the same approach is harder and worth knowing before trying: `sample(1)`
attributes every PE frame to one address inside Wine's unix ntdll, since that is the
last native frame before emulated code, and `winedbg`'s `bt all` walks every process
in the bottle. Reproduce on Windows where you can, and fall back to instrumenting the
shim — a recorder in the device vtable plus a watchdog that writes while wedged — for
what only happens under Wine.


### A validation failure that no longer aborts is still a failure

`MTL_ERROR_MODE=nslog` is what lets a group finish instead of wedging, but the call
the host rejected still failed, and the test carries on with whatever it got back. A
case whose expected result is zeroes can then **pass** on a resource the host refused
to create - a silently wrong result in place of a loud one.

So the runner counts them and says so:

* `host_validation_errors` per group and in the totals;
* `pass_in_groups_with_host_errors` in the totals - passes recorded in a group where
  the host rejected something, which have not been shown to mean anything.

Measured on `WGF11ResourceAccess` at one case per group: 1794 validation failures
survived, and 14 of the binary's 18 passes are in groups carrying one.

They are counted out of the runner's **own** stderr, which the `dxcts` launcher points
at `dxcts-out/host-stderr.log`. Metal and AGX write to the process's Unix fd 2 and
every test process inherits that from the runner, so redirecting the child's Windows
stderr does not move them - the per-group `.err` files stay empty however many there
were. That is the same Win32/Unix split that `MTL_ERROR_MODE` itself has to live with.

## Known limitations

Measured against D3DMetal under Wine.

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
  invented: `WGF11Filter` 38 groups, `WGF11SharedResources` 20, and 691 cases with 394
  skips on a one-case-per-group sample.

* **A geometry shader bound together with tessellation aborts the process.** D3DMetal
  builds a Metal mesh pipeline for a geometry shader and for tessellation, and cannot
  compile the geometry function when both are used at once: it logs "Failed to compile
  geometry function", leaves `meshFunction` nil, and hands the descriptor to Metal
  anyway, which fails validation and calls `abort()`. That abort lands on one of its own
  libdispatch workers, where Wine cannot turn a signal into an exception, so the process
  wedges instead of dying and nothing in it can intervene after the fact. All eight of
  `WGF11Precision`'s `LowPrecision\ShaderInstructions` groups end there.

  The d3dmetal layer therefore wraps every context and drops the draw before it can
  reach the pipeline, on exactly the condition that aborts — a geometry shader bound while a
  hull or domain shader is — so a draw using either alone is untouched. The case then
  fails on its readback like any other wrong result.

  Measured on `WGF11Precision`, every group, whole binary: **5 cases in 965s with
  eight groups timing out becomes 3640 cases in 144s, all executed, 1165 passing**,
  and the failures are `Result not within expected ranges` rather than setup errors.
  Across all 28 WGF D3D11 binaries at one case per group, the fixes here together
  take **13 timed-out groups to 2**, with three more now reported as crashes rather
  than as timeouts, 3391 cases all executed and passes 1289 -> 1292.

  That sweep is the right shape for catching regressions and the wrong one for
  measuring gains: at one case per group it cannot see a fix that lets a group run
  further, and it hides a crash that happens deeper in. `ResourceFormats\Formats`
  gaining 720 cases shows up in it as nothing at all. Run the group whole to measure
  a fix; run the sweep to check it broke nothing.

  A geometry shader with stream output layered over tessellation is legal D3D11 that
  `WGF11Tessellation` and `WGF11TiledResources` do exercise, and this drops those draws
  too, so a case that should pass can fail on readback instead. Set
  `DXCTS_D3DMETAL_DRAW_GUARDS=0` to tell that apart from a host defect; the report
  says per group how many draws were dropped, and whether the knob was on at all.

* **Alpha to coverage on an integer render target aborts the process.** D3D11 takes
  the combination - `WGF11Multisample\Coverage` enumerates
  `RT_resource_format=R10G10B10A2_UINT` with `A2CEnable` as a test parameter - but
  Metal refuses the pipeline (`alphaToCoverageEnabled requires a float/unorm pixel
  format`). Handled entirely by `MTL_ERROR_MODE`, with no shim: 8880 cases and 1808
  passes.

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
* **Some binaries are slow rather than hung.** `WGF11Multisample` averages ~15 s per
  case against D3DMetal and runs correctly throughout; give `--timeout` room rather
  than assuming a hang.
* **`--sample` is a percentage, not a fraction, and rounds up to at least one case per
  group.** `--sample 0.002` is therefore "one case per group", which exercises every
  group in the suite. The space is large: `WGF11Shader5x` enumerates 336,683 cases and
  `WGF11Blend` 302,919, so a full run is a very long one.
* **`--finish-ms`/`--query-ms` default to 1000 ms** against the framework's 10,000,
  bounding waits a host implementation never satisfies; raise them when the wait itself
  is under test.
* **TAEF buffers its console output until the process exits**, so a binary killed at
  its timeout yields no case records at all — only the fact that it timed out.
* **`D3DConf_12_Video` bugchecks the VMware display driver** (`0x1B
  PFN_SHARE_COUNT`) on the development VM, taking the machine down mid-sweep. It runs
  to completion on its own (14648 cases, no timed-out group), so the runner is not at
  fault; run it apart from the other TAEF binaries on that machine, or on a host whose
  display driver survives it.
* **Five groups run no cases, for three distinct reasons.**
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
  believes it. Reporting not-supported would let the test skip, but the d3d12 shim
  cannot: instrumenting the wrapper shows exactly ten `CheckFeatureSupport` calls
  reaching it, all from the scope fixture, and never `D3D12_FEATURE_D3D12_OPTIONS5` —
  so whatever builds that class's device does not pass through the shim's
  `D3D12CreateDevice`.
  Both `iFlip` groups take a null dereference inside `iflip.dll` itself: it does
  `pOutput->QueryInterface(&pDWMOutput)` for `IDXGIOutputDWM`, a DWM-private interface
  Wine does not implement, and calls `GetMultiplaneOverlayCaps` on the null result
  without checking (the faulting register holds 0x80004002, E_NOINTERFACE). The dxgi
  shim answers that query with an object whose methods report failure, which turns the
  crash into cases that run and fail on their own terms; iFlip runs 84 cases under
  WARP, where the real interface exists.
* Not reached by the shims: `DirectML.dll` (three DirectML/NPU binaries) and the
  optional TAEF test modes (`TE.TestMode.UAP`, `TE.TestMode.EtwLogger`,
  `Wex.Services.exe`), none of which are tests. Static closure over the staged x64
  tree: 222 of 228 images load.
