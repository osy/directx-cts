#!/usr/bin/env python3
"""Classify the kit's test binaries and write the suite catalog.

    ./tools/gen_catalog.py kit/WindowsKits10 > suites/graphics.jsonl

The catalog is ours - binary names, harness family, arguments - so it is committed;
the test-case listings it leads to are HLK-derived and are not.

Harness families, fingerprinted rather than hardcoded:
  wgf      the standalone framework: WTTLog plus the modern option vocabulary,
           selected by a positional backslash group path
  wgf9     the same framework's D3D9-era generation, whose groups are switches
           ("-Key", "-Key:1-50") listed only in its help dialog
  taef     a TAEF test DLL, run through TE.exe
  gtest    a GoogleTest binary
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pe as pemod  # noqa: E402

# Not runnable headlessly, and each for a different reason.
EXCLUDE = {
    "fnddraw.exe": "interactive DirectDraw application with its own UI",
    "DXGICrossAdapterScanOutPresent.exe": "helper spawned by D3DConf_11_Core",
    "HybridPerf.dll": "TAEF gate with no test methods; needs two adapters",
    "DirtyStateTestHelper.dll": "helper library",
    "perf_gputiming.dll": "helper library",
    "VerifySupportFiles.dll": "helper library",
    # Presents a gamma ramp and judges it through a hardware frame-capture device;
    # -NoCapture lets it run, but then it cannot evaluate what it presented.
    "DXGIGammaVM.exe": "requires a frame capture device",
}

# Needs a second GPU, so it cannot pass anywhere we run.
MULTI_ADAPTER = {"DXGIConf_HybridDList.dll", "DXGIConf_HybridCrossAdapterScanOut.dll",
                 "DXGIHybridPresent.exe"}

# DxgTaef's scope setup fixture asserts that DXGI enumerates an adapter flagged
# DXGI_ADAPTER_FLAG_SOFTWARE, and fails the whole binary without running a method when
# it does not.  The dxgi shim can describe the real adapter as software to satisfy it,
# but that is not free: D3DConf_11_Core's D3D11 device cache then takes a
# software-device path this host cannot service and hangs in setup, where without it its
# cases run.  So it is asked for per binary, by measurement, not turned on globally.
NEEDS_SOFTWARE_ADAPTER = {"D3DConf_12_Core.dll", "D3DConf_12_Video.dll"}

# iFlip queries IDXGIOutputDWM, a DWM-private interface Wine does not implement, and
# calls GetMultiplaneOverlayCaps on the null result without checking - so both its
# groups take the process down and run nothing.  The dxgi shim can answer that query
# with an object that reports failure, which is asked for per binary because it is
# the only one that needs it.
NEEDS_DWM_OUTPUT = {"iFlip.dll"}

# Data a binary reads from a fixed path outside its own directory, which the HLK job
# deploys there first.  DirectML.Test.ModelTests names its models relative to
# %TEMP%\\<Package> (the "Package" row of DirectMLModelTests.xml) and fails every
# method with "Invalid model path" when nothing has put them there.
DEPLOY = {
    "DirectML.Test.ModelTests.dll": ("MetacommandHlkTestData\\directml_model_tests",
                                     "%TEMP%\\directml.test.modeltests.0.0.4"),
}

# Binaries whose groups genuinely take longer than a run's default bound, so that a
# default suited to everything else does not report them as hung.  Measured, and
# deliberately modest: raising a bound only helps where the work is real, and a group
# that makes no progress costs the sweep the whole bound.
# D3DConf_12_Core: one D3DConf_12_0_MeshShaders group is 1110 s on an ARM64 WARP guest (six
# cases, the fifth CPU-bound for most of it), measured with the job's CPU time counted as
# liveness; 300 s cut every one of those groups at its fifth case.
SLOW_BINARIES = {"D3DConf_12_Core.dll": 1800}

API_PREFIX = [
    ("wgf11", "d3d11"), ("10l9", "d3d10level9"), ("dxgi", "dxgi"),
    ("d3dconf_12", "d3d12"), ("d3dconf_11", "d3d11"), ("d3dconf_9", "d3d9"),
    ("directml", "directml"), ("metacommand", "d3d12"), ("dstorage", "d3d12"),
    ("microsoft.dxgi", "dxgi"), ("iflip", "dxgi"), ("mapdefault", "d3d11"),
    ("dawn_end2end", "webgpu"), ("d3dprotection", "d3d11"), ("d3dsystemcheck", "d3d11"),
]


def classify(path: Path) -> tuple[str, int] | None:
    """(harness, taef method count) or None when the file is not a test."""
    try:
        img = pemod.PEFile(path)
    except (pemod.NotPE, OSError):
        return None
    blob = path.read_bytes()

    if b"gtest_list_tests" in blob:
        return "gtest", 0

    methods = blob.count(b"_TAEF_PinTestMethodInfo")
    imports = {i.dll.lower() for i in img.all_imports()}
    if methods or "te.common.dll" in imports:
        return "taef", methods

    if "wttlog.dll" not in imports and b"WTTLog.dll" not in blob:
        return None
    # The modern generation spells its options in mixed case and offers -GenerateCSV;
    # the D3D9-era one uses uppercase and numeric case ranges.
    if b"GenerateCSV" in blob and b"AdapterType" in blob:
        return "wgf", 0
    return "wgf9", 0


def api_of(name: str) -> str:
    low = name.lower()
    for prefix, api in API_PREFIX:
        if low.startswith(prefix):
            return api
    return "d3d9"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    kit = Path(sys.argv[1])
    base = kit / "Hardware Lab Kit" / "Tests" / "amd64" / "nttest" / "windowstest"
    roots = [base / "graphics" / "d3d" / "conf",
             base / "graphics" / "d3d" / "api",
             base / "graphics" / "web" / "dawn"]

    entries = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.iterdir()):
            if path.suffix.lower() not in (".exe", ".dll"):
                continue
            kind = classify(path)
            if kind is None:
                continue
            harness, methods = kind
            entry = {
                "binary": path.name,
                "harness": harness,
                "api": api_of(path.name),
            }
            if methods:
                entry["methods"] = methods
            if path.name in EXCLUDE:
                entry["skip"] = EXCLUDE[path.name]
            elif path.name in MULTI_ADAPTER:
                entry["skip"] = "requires a second adapter"
            if path.name in NEEDS_SOFTWARE_ADAPTER:
                entry["software_adapter"] = True
            if path.name in NEEDS_DWM_OUTPUT:
                entry["dwm_output"] = True
            if path.name in DEPLOY:
                entry["deploy_from"], entry["deploy_to"] = DEPLOY[path.name]
            if path.name in SLOW_BINARIES:
                entry["timeout_s"] = SLOW_BINARIES[path.name]
            entries.append(entry)

    for e in entries:
        print(json.dumps(e, separators=(",", ":")))

    counts: dict[str, int] = {}
    for e in entries:
        counts[e["harness"]] = counts.get(e["harness"], 0) + 1
    print(f"{len(entries)} binaries: "
          + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
