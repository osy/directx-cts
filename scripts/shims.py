"""The shim set, and the import rewrites that route binaries to it.

A rewrite is needed whenever the module we must replace cannot be reached by the
normal DLL search:

  * apiset names (api-ms-win-*) are resolved by Wine's apisetschema before the file
    search runs, so a DLL of that name beside the executable is never consulted;
  * d3d11/d3d12/dxgi are builtins, which win over a native DLL beside the executable.

Rewriting the importer's module name is length-preserving by construction, which is
why every replacement below matches its target's length exactly.  Ordinary names
absent from Wine (DEVOBJ) need no rewrite at all.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Shim:
    module: str          # file name produced by the build
    replaces: str | None # imported name to rewrite, or None if it loads by name
    why: str
    # Functions whose Wine implementation resolves but misbehaves, so importing one
    # is reason enough to rewrite even though nothing would fail to bind.
    behavioural: frozenset[str] = frozenset()
    # For a proxy that keeps the original's name: what the original is renamed to,
    # which is also what this module's forwarders target.
    renames_original: str | None = None
    # The framework resolves this module with LoadLibrary on a string literal as well
    # as importing it, so the literal has to be retargeted too.
    loaded_by_name: bool = False
    # Retarget that literal only in binaries the catalog runs under this harness: the
    # name is common enough that elsewhere the shim is wanted through the import
    # table alone, or not at all.
    literal_harness: str | None = None
    # Not a compatibility shim: needed on every target, because it is how results are
    # collected rather than how a module is made to load.
    every_target: bool = False

    def __post_init__(self) -> None:
        if self.replaces and len(self.replaces) != len(self.module):
            raise ValueError(
                f"import rewrite must preserve length: {self.replaces!r} "
                f"({len(self.replaces)}) -> {self.module!r} ({len(self.module)})")


SHIMS: list[Shim] = [
    Shim("DEVOBJ.dll", None,
         "absent from Wine; forwards to setupapi"),
    Shim("dxcts_d3dkmt_compat_shim_v1.dll", "api-ms-win-dx-d3dkmt-l1-1-0.dll",
         "D3DKMTSubmitCommand/SubmitCommandToHwQueue/EnumAdapters3 live in win32u, "
         "not the gdi32 the apiset resolves to; and Wine's OpenAdapterFromDeviceName "
         "and QueryAdapterInfo reject the adapter, emptying every -AdapterType subset",
         frozenset({"D3DKMTOpenAdapterFromDeviceName", "D3DKMTQueryAdapterInfo"}),
         every_target=True),
    Shim("dxgdi.dll", "gdi32.dll",
         "the DXGI and D3D9-era binaries import the D3DKMT adapter queries from "
         "gdi32 directly rather than through the apiset, so the d3dkmt shim never sees "
         "them and Wine's answers empty every -AdapterType subset; this forwards those "
         "three to the d3dkmt shim and everything else back to gdi32.  The D3D9-era "
         "framework also loads gdi32 by name for D3DKMTOpenAdapterFromHdc, a stub in "
         "Wine, and without an adapter caps every test at Direct3D 9.0",
         frozenset({"D3DKMTOpenAdapterFromDeviceName", "D3DKMTQueryAdapterInfo"}),
         loaded_by_name=True, literal_harness="wgf9"),
    Shim("dxc11.dll", "d3d11.dll",
         "CreateDirect3D11DeviceFromDXGIDevice has no Wine equivalent and five DXGI "
         "tests import it; the device needs wrapping to answer the interface the HLK "
         "probes it for; and device creation is where a backend layer is applied",
         frozenset({"D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"}),
         loaded_by_name=True, every_target=True),
    Shim("dxcg.dll", "dxgi.dll",
         "adapter enumeration has to be answerable per binary: a software adapter for "
         "DxgTaef's scope fixture, which asserts one exists and otherwise fails every "
         "method in the binary without running it; WARP only for the reference arm, "
         "since TAEF has no -Src switch and takes the first adapter DXGI reports; and "
         "an IDXGIOutputDWM stub for iFlip",
         frozenset({"CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2"}),
         loaded_by_name=True, every_target=True),
    Shim("dxc12.dll", "d3d12.dll",
         "D3D12GetInterface is in d3d12core, not d3d12; PIX hooks stubbed; and a run "
         "asking for WARP needs the null adapter substituted, since "
         "D3D12CreateDevice(NULL) takes the default adapter without enumerating",
         frozenset({"D3D12CreateDevice", "D3D12GetInterface"}),
         loaded_by_name=True, every_target=True),
    Shim("dxc9.dll", "d3d9.dll",
         "every D3D9-era framework resolves d3d9 ordinal 16 "
         "(Direct3D9ForceHybridEnumeration) and gives up on the device without it; "
         "Wine's d3d9 stops at 15.  Everything else forwards to the platform's d3d9",
         loaded_by_name=True),
    Shim("dxmsvc.dll", "msvcrt.dll",
         "the x86 binaries import the _libm_sse2_*_precise maths entry points, which "
         "Wine's msvcrt has only under their __libm_sse2_* names (and sqrt not at "
         "all), so they stop at start; everything else forwards to msvcrt"),
    Shim("dxctspwr.dll", "powrprof.dll",
         "Wine implements none of the Power*ValueIndex accessors DXGIHybridPresent "
         "reads its GPU preference through, and the two it does export return success "
         "without writing the caller's out-parameter"),
    Shim("dxcts_power_setting_compat_shim.dll",
         "api-ms-win-power-setting-l1-1-0.dll",
         "DXGIHybridPresent reaches the power accessors through this apiset, which "
         "the schema resolves before any file search, so the powrprof shim cannot be "
         "reached by its own name; forwards to it"),
    Shim("dxcdc.dll", "dcomp.dll",
         "D3DConf_11_Core statically imports dcomp ordinal 2005 "
         "(CreatePresentationFactory on Windows); Wine's dcomp stops at 1042, so the "
         "whole 39-method D3D11.3 binary fails to load"),
    Shim("dxcts_crt_private_compat_shim.dll", "api-ms-win-crt-private-l1-1-0.dll",
         "Wine's ucrtbase lacks __C_specific_handler_noexcept, which "
         "d3ddriververifier, d3d11_3sdklayers and dxgidebug all bind; it lives in "
         "vcruntime140"),
    Shim("dxctsole.dll", "oleaut32.dll",
         "TE.Model binds the *_User*64 marshalling helpers; Wine exports only the "
         "unsuffixed names, which are the 64-bit implementations on x64"),
    Shim("WTTLog.dll", None,
         "records the per-case event stream; both HLK framework generations log "
         "through CWTTLogger, so it is the one results channel covering them, and the "
         "only way to tell a case that never ran from one that failed",
         renames_original="dxctswtt.dll", every_target=True),
    Shim("dxrpc4.dll", "rpcrt4.dll",
         "TE.Common/TE.Model import Conduit.Broker statically, and it binds six RPC "
         "entry points Wine lacks; stubbed because only remote execution calls them"),
]

# Backend compatibility layers: one DLL per host D3D implementation, staged on every
# target and loaded by the d3d11/d3d12 shims only when the runner selects it
# (--backend NAME -> DXCTS_BACKEND=NAME).  Nothing imports them, so no rewrite.
BACKENDS: list[str] = ["dxcts_backend_d3dmetal.dll"]


# Every version of an apiset resolves through one schema entry, so retargeting the
# importer means rewriting whichever spelling it used. These are the versions the kit's
# binaries import; check with
#   ./tools/check_wine.py --summary staging/<arch> | grep d3dkmt
# after a kit update, since a name absent from this list is left pointing at gdi32.
D3DKMT_APISETS = [f"api-ms-win-dx-d3dkmt-l1-1-{n}.dll" for n in (0, 1, 2, 6)]


def behavioural() -> dict[str, frozenset[str]]:
    """{shim module name: functions that force a rewrite regardless of binding}"""
    return {s.module: s.behavioural for s in SHIMS if s.behavioural}


def rewrites() -> dict[str, str]:
    """{imported module name: shim module name}"""
    out: dict[str, str] = {}
    for shim in SHIMS:
        if shim.replaces is None:
            continue
        if shim.replaces.startswith("api-ms-win-dx-d3dkmt"):
            for name in D3DKMT_APISETS:
                out[name] = shim.module
        else:
            out[shim.replaces] = shim.module
    return out
