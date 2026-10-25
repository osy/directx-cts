# directx-cts

A free-standing conformance and regression suite for Direct3D implementations, built
from the test content of Microsoft's Windows Hardware Lab Kit (HLK).

The HLK holds the largest body of D3D9-D3D12 and DXGI conformance tests there is, but
it ships locked to a Windows-only controller and job scheduler. The test binaries
themselves are self-contained - they enumerate their own cases and judge their own
results - so this project supplies what is missing around them:

* a reproducible way to fetch the kit, with no Windows and no installer;
* compatibility shims, so the binaries load and run under unmodified Wine;
* optional **backend layers** that work around what one host D3D implementation gets
  wrong (today Apple's D3DMetal), applied the same way whether that implementation
  runs in the test process or behind a guest driver;
* a runner, `dxcts.exe`, that drives every test family the kit contains and writes one
  machine-readable report, which tells a case that ran and failed apart from one that
  never ran.

It runs on Wine (including CrossOver) and on Windows - real machines or VMs - with the
x86, x64 or arm64 build of the tests, whichever the target runs.
**No HLK content belongs in this repository**: `kit/`, `staging/` and `build/` are
generated and ignored.

## Quick start

Requirements: Python 3.11+, `7z` and `msitools` for the kit
(`brew install p7zip msitools`), CMake with Ninja, and a Windows cross-compiler -
mingw-w64 for x86/x64 (`brew install mingw-w64`), or [llvm-mingw] or `zig`
(`brew install zig`) for all three architectures. On Windows, MSVC builds without the
toolchain file.

    # 1. the kit (~1.7 GB, verified against Microsoft's manifest)
    ./scripts/fetch_kit.py
    ./scripts/extract_kit.py

    # 2. the shims, backend layers and runner, for one architecture
    cmake -B build/<arch> -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake -DDXCTS_ARCH=<arch>
    cmake --build build/<arch>

    # 3. one self-contained directory to copy to the target
    ./scripts/stage.py --arch <arch> --target <wine|windows>   # -> staging/<arch>

Two independent choices:

* **`<arch>`** - `x86`, `x64` or `arm64` - is which build of the tests to run: the
  kit ships each binary for all three. Pick the one the target runs natively (x64 on an
  x64 Windows machine or x64 Wine - CrossOver on Apple silicon runs x64 under Rosetta;
  arm64 on arm64 Windows or arm64 Wine), or one it emulates (x64 or x86 on arm64
  Windows) to test that path. The D3D9-era (`wgf9`) binaries need the x86 staging on
  every target: their framework needs `d3dx8d.dll`, which only the x86 kit ships.
* **`--target`** is the platform: `wine` stages every shim, the import rewrites Wine
  needs and the `dxcts` launcher; `windows` stages only the shims every platform needs,
  since Windows provides the rest.

Then, from the staged directory on the target:

    # Wine (CrossOver included): always through the generated launcher
    ./dxcts run --per-group --out report.json

    # Windows: from the interactive console session
    dxcts.exe run --per-group --out report.json

On Wine, `dxcts` - the launcher staging writes beside `dxcts.exe` - sets
`MTL_ERROR_MODE=nslog` and the host-log path before Wine starts, and inside a CrossOver
bottle derives the bottle and its `C:` path from where it was copied to. See
[docs/running.md](docs/running.md).

A CrossOver run against D3DMetal, with the backend layer and its exclusion list:

    ./dxcts run --per-group --backend d3dmetal --exclusion crossover --out report.json

[llvm-mingw]: https://github.com/mstorsjo/llvm-mingw/releases

## Commands

    dxcts list  [selection]     list the selected binaries, with skips and exclusions
    dxcts run   [options]       run them and write the report
    dxcts probe                 describe the target and the workarounds that apply, as JSON
    dxcts help                  the usage (also --help, which CrossOver's wine takes for itself)

### Selection

| Option | Meaning |
|---|---|
| `--catalog PATH` | catalog to use (default `suites/graphics.jsonl` beside `dxcts.exe`) |
| `--api NAME` | only binaries of this API: `d3d9`, `d3d10level9`, `d3d11`, `d3d12`, `dxgi`, `directml`, `webgpu` |
| `--harness NAME` | only this framework family: `wgf`, `wgf9` (D3D9-era), `taef`, `gtest` |
| `--binary NAME` | only this binary (a path is accepted; its last component is matched) |
| `--group PATH` | one group: a WGF group path, a D3D9-era group key, a TAEF class or a gtest filter |
| `--sample PERCENT` | a deterministic sample (the WGF family only; the others run every case and the report says so): a percentage, rounded up to at least one case per group, so `0.002` runs one case of every group |
| `--seed N` | sample seed (default 1) |
| `--include-skipped` | also run the binaries the catalog marks as unrunnable anywhere |
| `--exclusion LIST` | leave out what an exclusion list names: a path, or the name of a list in `suites/exclusions` (`warp`, `d3dmetal`, `crossover`, `dxmt`, `crossover-dxmt`); see [docs/exclusions.md](docs/exclusions.md) |

### Execution

| Option | Default | Meaning |
|---|---|---|
| `--per-group` | off | one process per group (per TAEF class, per gtest suite), so a group that wedges or crashes costs only itself. Recommended for every real run |
| `--timeout SECONDS` | 7200 | limit per spawned process - each group under `--per-group`, each binary otherwise. Given explicitly, it also overrides the catalog's per-binary value |
| `--stall SECONDS` | 600 | end a process whose output has not grown for this long and report it `wedged`; `0` disables |
| `--slow SECONDS` | 60 | name a group that runs longer than this in the report (`slow`), without failing it |
| `--finish-ms MS` | 1000 | how long the WGF framework waits for pending GPU work (its own default is 10000) |
| `--query-ms MS` | 1000 | how long it waits for a query result (its own default is 10000) |
| `--src NAME` | | the WGF framework's device source: `REF`, `HW`, `SW`, `NULL` or `WARP`. `WARP` also hides every other adapter from the TAEF and gtest binaries; see [docs/warp.md](docs/warp.md) |
| `--adapter-type T` | `all` | the WGF framework's adapter class: `all`, `fullhw`, `renderonlyhw`, `fullsw`, `renderonlysw` |
| `--11on12` | off | run the D3D11 WGF binaries on D3D11On12, so the D3D12 implementation is under test (the kit's own D3D12 job list runs them this way) |
| `--backend NAME` | none | apply a backend layer, `dxcts_backend_NAME.dll` (today `d3dmetal`); see [docs/backends.md](docs/backends.md) |
| `--no-patches` | off | turn every workaround knob off, to measure what they are worth |
| `--out PATH` | stdout | where the JSON report goes. Progress is printed to stdout only when the report goes to a file |

## Environment

A user only needs the variables in this table; the runner sets the rest.

| Variable | Read by | Meaning |
|---|---|---|
| `MTL_ERROR_MODE` | Metal, set by the launcher | default `nslog`: Metal reports a validation failure and returns instead of calling `abort()`. Recorded in the report |
| `DXCTS_HOST_LOG` | launcher, runner | the file the launcher sends the host's own stderr to (default `dxcts-out/host-stderr.log`), and the runner counts Metal validation errors out of |
| `WINE` | launcher | the wine to use (default: CrossOver's inside a bottle, otherwise `wine` on `PATH` with `WINEPREFIX` as set) |
| `DXCTS_<SHIM>_<WHAT>=0` or `=1` | shims, backend layers | turn one workaround knob off, or force it on; the knobs are listed in [docs/shims.md](docs/shims.md#knobs) and [docs/backends.md](docs/backends.md#knobs) |
| `DXCTS_NO_PATCHES=1` | shims, backend layers | every knob off (what `--no-patches` sets) |

Set by the runner, per run or per binary: `DXCTS_BACKEND` (from `--backend`),
`DXCTS_EVENTS` (where the WTTLog proxy writes a process's case events),
`DXCTS_SOFTWARE_ADAPTER` and `DXCTS_DWM_OUTPUT` (requests from the catalog) and
`DXCTS_WARP_ONLY` (from `--src WARP`). See [docs/shims.md](docs/shims.md#requests).

## Output

The report (`--out`) holds, per binary, every case with `executed` and `result`
(`pass`, `fail`, `skip`, `blocked`, `warn`, `incomplete` - started and never ended -
or `not-run`); each group's outcome (timed out, wedged, hung in its exit, crashed and
resumed, slow, host validation errors, its dominant failure reasons); what an
exclusion list left out and why; and totals. `target` and `workarounds` say what the
run ran on and what was applied to it. The schema, and the tools that compare and
summarise reports, are in [docs/report.md](docs/report.md).

Each process's own output - the framework's log, stdout, stderr and event stream - is
kept in `dxcts-out/` beside the runner, named after the binary and group.

## Layout

    kit.toml        the pinned HLK release and the installer payloads it needs
    scripts/        kit acquisition (fetch, extract) and staging
    suites/         the test catalog, and the exclusion lists in exclusions/
    src/runner/     dxcts.exe
    src/shims/      the compatibility shims, and the backend layers (backend_*/)
    src/common/     what they share: COM wrapping and interface tables, knobs, the
                    backend hand-off, the WARP lookup, the event stream, JSON, DllMain
    tools/          PE inspection, generated-input refresh, the static Wine load
                    check, report comparison
    cmake/          toolchain files
    docs/           the documentation below

## Documentation

* [docs/kit.md](docs/kit.md) - fetching and extracting the kit, adopting a new one,
  the catalog and the other inputs generated from the kit
* [docs/shims.md](docs/shims.md) - how the binaries are made to load under Wine, each
  shim and why, the knobs and requests, the static load check
* [docs/backends.md](docs/backends.md) - backend layers, the D3DMetal layer and its
  knobs, and `MTL_ERROR_MODE`
* [docs/running.md](docs/running.md) - how each test family is run, split and resumed;
  timeouts, stalls and hangs; running on Windows; debugging a timeout
* [docs/exclusions.md](docs/exclusions.md) - exclusion lists: format, how each family
  is told, what the report records
* [docs/report.md](docs/report.md) - the report schema, and the tools that read it
* [docs/warp.md](docs/warp.md) - the WARP reference arm
* [docs/limitations.md](docs/limitations.md) - known limitations
