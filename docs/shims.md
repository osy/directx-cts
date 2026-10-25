# Shims

The shims are what let the HLK binaries load and start, and report results, on a
platform that is not the Windows build they were written for. They never change what a
test measures: workarounds for a host D3D implementation's defects live in a backend
layer instead ([backends.md](backends.md)).

## How a shim is reached

Wine is never modified. Most shims are ordinary DLLs staged beside the tests, where
the application directory wins the search. Two cases cannot be reached that way, and
those binaries get a **length-preserving import-name rewrite** applied to the staging
copy:

* apiset names (`api-ms-win-*`) are resolved by Wine's `apisetschema.dll` before the
  file search runs, so a DLL of that name beside the executable is never consulted;
* `d3d9`, `d3d11`, `d3d12`, `dxgi`, `dcomp`, `gdi32`, `msvcrt`, `oleaut32`, `powrprof`
  and `rpcrt4` are Wine builtins, which win over a native DLL in the application
  directory.

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
| `dxgdi.dll` | import rewrite; LoadLibrary rewrite (wgf9) | the DXGI and D3D9-era binaries import `D3DKMTOpenAdapterFromDeviceName`/`QueryAdapterInfo`/`CloseAdapter` from gdi32 directly rather than through the apiset, so the d3dkmt shim never sees them and Wine's answers empty every `-AdapterType` subset ("adapter subset of size (0)"); a generated forwarder (`tools/gen_gdi32_def.py`) that sends those to the d3dkmt shim and everything else back to gdi32. The D3D9-era framework also loads `L"gdi32.dll"` by name for `D3DKMTOpenAdapterFromHdc`, a stub in Wine, and without an adapter caps every test at Direct3D 9.0 ("the maximum version for this OS/architecture is 9.0"), so in the `wgf9` binaries that literal is retargeted too |
| `dxc11.dll` | import rewrite | CreateDirect3D11DeviceFromDXGIDevice has no Wine equivalent and five DXGI tests import it; the device needs wrapping to answer the interface eight binaries probe it for; and device creation is where a backend layer is applied |
| `dxc12.dll` | import rewrite | D3D12GetInterface is in d3d12core, not d3d12; PIX hooks stubbed; WARP substituted for a null adapter; device creation is where a backend layer is applied |
| `dxc9.dll` | LoadLibrary rewrite | every D3D9-era framework resolves d3d9 ordinal 16 (`Direct3D9ForceHybridEnumeration`) and gives up on the device without it; Wine's d3d9 stops at 15. A no-op there, everything else forwarded to the platform's d3d9 |
| `dxmsvc.dll` | import rewrite | the x86 binaries import the `_libm_sse2_*_precise` maths entry points, which Wine's msvcrt has only as `__libm_sse2_*` (and sqrt not at all), so they stop at start; a generated forwarder (`tools/gen_msvcrt_def.py`) mapping those and sending everything else back to msvcrt |
| `dxcg.dll` | import rewrite | adapter enumeration has to be answerable per binary: a software adapter for DxgTaef's scope fixture, WARP only for the reference arm, an IDXGIOutputDWM stub for iFlip |
| `dxcdc.dll` | import rewrite | D3DConf_11_Core statically imports dcomp ordinal 2005 (CreatePresentationFactory on Windows); Wine's dcomp stops at 1042, so the whole 39-method D3D11.3 binary fails to load |
| `dxcts_power_setting_compat_shim.dll` | import rewrite | DXGIHybridPresent reaches the power accessors through this apiset, which the schema resolves before any file search, so the powrprof shim cannot be reached by its own name |
| `dxctspwr.dll` | import rewrite | Wine implements none of the Power*ValueIndex accessors DXGIHybridPresent reads its GPU preference through, and the two it does export return success without writing the caller's out-parameter |
| `dxcts_crt_private_compat_shim.dll` | import rewrite | Wine's ucrtbase lacks __C_specific_handler_noexcept, which d3ddriververifier, d3d11_3sdklayers and dxgidebug all bind; it lives in vcruntime140 |
| `dxctsole.dll` | import rewrite | TE.Model binds the *_User*64 marshalling helpers; Wine exports only the unsuffixed names, which are the 64-bit implementations on x64 |
| `WTTLog.dll` | renamed original | records the per-case event stream; both HLK framework generations log through CWTTLogger, so it is the one results channel covering them, and the only way to tell a case that never ran from one that failed |
| `dxrpc4.dll` | import rewrite | TE.Common/TE.Model import Conduit.Broker statically, and it binds six RPC entry points Wine lacks. Conduit binds to its own broker at startup, so `RpcBindingCreateW` is rebuilt on the string-binding API Wine has; the server-side ones refuse, since only remote execution calls them |

## Knobs

Every workaround for a defect is one switch, so what it is worth can be measured rather
than argued about — see `src/common/knob.h`. `DXCTS_<SHIM>_<WHAT>=0` turns one off,
`=1` forces it on, and `dxcts run --no-patches` (or `DXCTS_NO_PATCHES=1`) turns every
one off at once; the report records that it was set.

The shims beside the tests only make a binary load and start: their knobs are for
things the *platform* lacks, so their defaults differ between Wine and Windows, and on
Windows every one is off.

| Knob | Wine | Windows | What it does |
|---|---|---|---|
| `DXCTS_D3D11_DEVICE_PROBE_IFACE` | on | off | answer the undocumented device interface eight binaries query |
| `DXCTS_D3D11_LOAD_VERIFIER` | on | off | load D3DDriverVerifier.dll before the device exists |
| `DXCTS_D3D11_PIN_CRYPTO` | on | off | keep bcrypt and crypt32 loaded (the HLK's shader compiler loads and frees both around every hash; under Wine each cycle leaks dyld state) |
| `DXCTS_D3DKMT_ADAPTER_NAME_FALLBACK` | on | off | fall back to the primary GDI display name |
| `DXCTS_D3DKMT_ADAPTER_INFO_FALLBACK` | on | off | answer the adapter-type query the classifier needs |

Everything the host D3D *implementation* gets wrong lives in a backend layer instead
([backends.md](backends.md)), whose knobs are on whenever the layer is selected, on
either platform.

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

## On Windows

`--target windows` stages only the shims needed on every target: the WTTLog proxy,
which is the results channel rather than a compatibility fix; the d3dkmt, dxgi and
d3d12 shims, which a WARP run's adapter selection goes through; and the d3d11 shim,
which is where a backend layer is applied. The rest are left out, because Windows
provides those modules and a shim in the application directory would shadow the real
one. Every workaround inside a shim that is staged there is off by default (see
[Knobs](#knobs)). The backend layers are staged on every target too, and do nothing
unless a run names one.

## Requests

Separate from the knobs, three things are *requests* the runner makes per binary or per
run, from the catalog and its own options, rather than platform judgements. A request
says what a binary needs in order to be testable at all; a knob says whether a defect is
worked around. A knob never vetoes a request.

| Request | Set when | What the dxgi shim (and, for WARP, the d3d12 and d3dkmt shims) does |
|---|---|---|
| `DXCTS_SOFTWARE_ADAPTER` | the catalog's `software_adapter` | describes a real adapter as `DXGI_ADAPTER_FLAG_SOFTWARE` to the first two factories a process creates: DxgTaef's scope fixture asserts one exists and otherwise fails every method of the binary without running it, while its D3D11 device cache - the third factory - hangs if it finds one |
| `DXCTS_DWM_OUTPUT` | the catalog's `dwm_output` | answers the DWM-private `IDXGIOutputDWM` query with an object whose methods fail; iFlip calls through the null result otherwise ([limitations.md](limitations.md)) |
| `DXCTS_WARP_ONLY` | `--src WARP`, for TAEF and gtest binaries | on Windows, where WARP exists: enumerates only WARP, substitutes WARP for a null adapter in `D3D12CreateDevice`, and answers the driver-version query with WARP's ([warp.md](warp.md)) |

## Static load check

Check what would fail to load under Wine before running anything:

    ./tools/check_wine.py --arch <arch> --summary --app-dir staging/<arch> staging/<arch>

This resolves every import against Wine's builtin exports, apiset redirection
included. It is static: a clean result means the loader will accept the image, not
that the test passes. Set `WINE_DLL_DIR` if the Wine installation is somewhere it does
not look.

`--arch` (`x86`, `x64` or `arm64`, default `x64`) picks which of Wine's DLL directories
to resolve against (`i386-windows`, `x86_64-windows`, `aarch64-windows`), and must
match the staging; `--delay-loads` also reports delay-load imports
(latent rather than load failures), and `--summary` groups the result by cause. Over a
current x64 staging, 226 of 232 images load; the six that do not are `DirectML.dll`'s
three importers and the optional TAEF modes (`TE.TestMode.UAP`,
`TE.TestMode.EtwLogger`, `Wex.Services.exe`), none of which is a test.
