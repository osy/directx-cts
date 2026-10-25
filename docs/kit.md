# The kit, the build and the catalog

## Fetching

    ./scripts/fetch_kit.py --plan          # show what would be downloaded
    ./scripts/fetch_kit.py                 # ~1.7 GB into kit/Installers
    ./scripts/fetch_kit.py --no-optional   # skip the job/requirement definitions
    ./scripts/extract_kit.py               # -> kit/WindowsKits10
    ./scripts/extract_kit.py --force       # re-extract over an existing tree

Both take `--kit-dir` (default `kit/`) and `--config` (default `kit.toml`). Requires
`7z` and `msitools` (macOS: `brew install p7zip msitools`).

Fetching needs no Windows and runs no installer. The bootstrapper behind the pinned
fwlink is a WiX Burn bundle; its manifest gives every payload's size and SHA-1, and each
MSI's external cabs come from that MSI's own Media table, so only the bytes the
selected MSIs need are pulled. Everything is verified on download. One TAEF MSI carries
the runtime for all three architectures, so no per-architecture TAEF package is
fetched.

`kit.toml` lists the MSIs (`[msi] required` and `optional`, the latter being job and
requirement definitions that are not needed to run anything), each with the files it
is there for. Extraction flattens every MSI's `Windows Kits/10` prefix into one tree,
the layout the HLK controller would install, and ends with a closure check
(`[verify]`): if a future kit moves a file between packages, extraction fails naming
the missing paths instead of producing a tree that breaks at run time.

To adopt a newer HLK, change `fwlink` and `version` in `kit.toml`. A version mismatch
against the live manifest is reported rather than silently accepted. Then refresh the
generated inputs below and rebuild.

## Building

    cmake -B build/<arch> -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.cmake -DDXCTS_ARCH=<arch>
    cmake --build build/<arch>

`<arch>` is `x86`, `x64` or `arm64`. The toolchain file takes the first cross-compiler
it finds: llvm-mingw (all three), mingw-w64 (x86 and x64), then `zig cc` (all three,
through the wrappers in `cmake/toolchains/zig`). On Windows with MSVC, configure
without the toolchain file and pass the `DXCTS_ARCH` that matches the compiler; a
mismatch is refused at configure time, since the WTTLog proxy's export list differs by
architecture.

Everything lands in `build/<arch>/bin`: the shims, the backend layers and `dxcts.exe`.

## Staging

    ./scripts/stage.py --arch <arch> --target <wine|windows>   # -> staging/<arch>

| Option | Meaning |
|---|---|
| `--arch` | `x64` (default), `x86` or `arm64`: which build of the tests, and of the shims and runner. It is independent of the platform: pick what the target runs, natively or under emulation |
| `--target` | the platform, independent of the architecture. `wine` (default): every shim, the import rewrites Wine needs (checked against that architecture's Wine DLLs) and the `dxcts` launcher. `windows`: only the shims needed on every platform, no launcher |
| `--kit-dir` | the kit (default `kit/`) |
| `--build-dir` | where the build is (default `build/<arch>/bin`) |
| `--out` | the staging directory (default `staging/<arch>`) |
| `--no-rewrite` | stage the shims but apply no import rewrites |

Staging produces one flat directory - the test binaries, their support DLLs and data,
TAEF's MinTe runtime, the shims, the backend layers, `dxcts.exe` and `suites/` - to
copy into a Wine bottle or a Windows machine and run from there. The kit is copied,
never modified: rewrites apply to the staging copy. `dxcts-staging.json` records the
kit version, the shims staged, the backend layers, and every binary each rewrite
touched. How the shims and the rewrites work is in [shims.md](shims.md).

## The catalog

`suites/graphics.jsonl` lists every test binary in the kit, one flat JSON object per
line:

| Key | Meaning |
|---|---|
| `binary` | the file name in the staging directory |
| `harness` | `wgf`, `wgf9`, `taef` or `gtest`: how the runner drives it ([running.md](running.md)) |
| `api` | what it tests: `d3d9`, `d3d10level9`, `d3d11`, `d3d12`, `dxgi`, `directml`, `webgpu` |
| `skip` | why it cannot run anywhere (a helper library, a second adapter or a capture device it needs); left out unless `--include-skipped` |
| `software_adapter` | the binary needs DXGI to enumerate a software adapter ([shims.md](shims.md#requests)) |
| `dwm_output` | the binary needs the DWM-private output interface answered ([shims.md](shims.md#requests)) |
| `deploy_from`, `deploy_to` | data the binary reads from a fixed path outside the staging tree, which the HLK job would put there: the runner copies `deploy_from` (relative to the staging directory) to `deploy_to` (environment variables expanded) before running it |
| `timeout_s` | the seconds this binary's groups actually need, where that is more than the default; a measured cost, not a way to hide a hang, and an explicit `--timeout` overrides it |

## Refreshing generated inputs

These are derived from the kit and regenerated when it changes:

    ./tools/gen_catalog.py kit/WindowsKits10 > suites/graphics.jsonl
    ./tools/gen_wttlog_def.py kit/WindowsKits10
    ./tools/gen_gdi32_def.py kit/WindowsKits10
    ./tools/gen_msvcrt_def.py kit/WindowsKits10

* The catalog classifies each binary by fingerprint (TAEF metadata, gtest strings, the
  WTTLog import and the option vocabulary of each WGF generation) rather than by a
  hardcoded list; the skips and requests above are tables in `gen_catalog.py`, each
  entry with the reason it was measured to need.
* The WTTLog proxy's `.def` is taken from the kit's own import tables, because
  `CWTTLogger`'s exported names are MSVC-mangled, the mangling differs per
  architecture, and part of the surface is imported by ordinal.
* The gdi32 and msvcrt forwarders' `.def`s are every gdi32 and msvcrt import the kit
  makes, so a binary that starts importing a new one still loads.
