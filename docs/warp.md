# The WARP reference arm

Windows ships WARP, a complete software rasterizer for D3D11 and D3D12. Running the same
staging tree against it answers "is this the implementation under test, the test, or
this harness?" without changing anything else:

    dxcts run --per-group --src WARP --exclusion warp --out warp.json

Run it on a Windows machine with no other GPU driver - a VM with only the Microsoft
Basic Display adapter is ideal - so nothing reaches real hardware, from the console
session ([running.md](running.md#on-windows)), with the D3D9-era binaries from an x86
staging.

On 2026-10-18, a full sweep of that kind on an arm64 Windows VM ran 63 binaries and
1,769,609 cases with no group timing out or wedging; 17 crashes of WARP's own shader JIT
were resumed past by the runner. Its exclusion list has one entry: D3D9 has no WARP
device, so on such a machine D3D9 enumerates no adapter and every D3D9-era binary
blocks before its first case.

## What `--src WARP` does

Asking for WARP takes three things, because each layer can bypass the one below:

* `-Src:WARP` for the WGF binaries, which is the framework's own device selection;
* hiding every adapter but WARP from `EnumAdapters`, `EnumAdapters1` and
  `EnumAdapterByGpuPreference`, since TAEF has no equivalent switch and DxgTaef takes
  the first adapter DXGI reports;
* substituting the WARP adapter inside `D3D12CreateDevice`, because the D3D12 suites
  pass a null adapter and the runtime then takes the default one without enumerating
  anything at all.

The d3dkmt shim also answers `D3DKMTQueryAdapterInfo`'s driver-version query with
WARP's own version, since the tests are not running on the display driver.

`--src WARP` hides the other adapters only from the TAEF and gtest binaries, which have
no way to ask for a device themselves (`DXCTS_WARP_ONLY`). The WGF binaries get
`-Src:WARP` and keep the enumeration intact: their DXGI present tests look up the
adapter that owns the monitor's output, and WARP has no outputs, so with everything else
hidden every one of them would block before its first case ("adapter with no outputs").
It matters: on a machine with a GPU, missing any one of the three leaves the suites
running on that GPU's driver instead.
