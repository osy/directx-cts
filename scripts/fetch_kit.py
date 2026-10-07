#!/usr/bin/env python3
"""Fetch the pinned Windows HLK installer payloads from Microsoft.

No Windows and no installer run required: the HLK bootstrapper is a WiX Burn bundle,
so we take its manifest, pick the MSIs we need, resolve each one's external cabs from
its own Media table, and pull exactly those files off the CDN.

    ./scripts/fetch_kit.py                 # fetch into ./kit
    ./scripts/fetch_kit.py --plan          # show what would be fetched, download nothing
    ./scripts/fetch_kit.py --no-optional   # skip the job/requirement definitions

Nothing here is ever committed; see .gitignore.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from hlkkit import (  # noqa: E402
    DEFAULT_KIT_DIR, Fail, Payload, download, eprint, extract_burn_manifest,
    load_config, msi_external_cabs, parse_manifest, payload_url, resolve_redirect,
    verify,
)

FWLINK = "https://go.microsoft.com/fwlink/?linkid={}"


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    raise AssertionError("unreachable: the GB branch always returns")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kit-dir", type=Path, default=DEFAULT_KIT_DIR,
                    help="destination (default: ./kit)")
    ap.add_argument("--plan", action="store_true",
                    help="resolve and print the payload list, download nothing")
    ap.add_argument("--no-optional", action="store_true",
                    help="skip the optional job/requirement definition packages")
    ap.add_argument("--config", type=Path, default=None)
    args = ap.parse_args()

    cfg = load_config(args.config)
    pinned = cfg["kit"]["version"]
    kit_dir: Path = args.kit_dir
    installers = kit_dir / "Installers"

    # 1. fwlink -> HLKSetup.exe, and the CDN directory it lives in.
    link = FWLINK.format(cfg["kit"]["fwlink"])
    eprint(f"resolving {link}")
    setup_url = resolve_redirect(link)
    if not setup_url.lower().endswith(".exe"):
        raise Fail(f"fwlink did not resolve to an .exe: {setup_url}")
    base = setup_url.rsplit("/", 1)[0]
    eprint(f"  -> {setup_url}")
    eprint(f"  CDN base: {base}")

    # 2. The bootstrapper, and the Burn manifest inside it.
    setup_exe = kit_dir / "HLKSetup.exe"
    if not setup_exe.is_file():
        eprint(f"downloading {setup_exe.name}")
        download(setup_url, setup_exe)
    manifest_bytes = extract_burn_manifest(setup_exe, kit_dir / ".work")
    (kit_dir / "BurnManifest.xml").write_bytes(manifest_bytes)

    version, payloads = parse_manifest(manifest_bytes)
    eprint(f"manifest: version {version}, {len(payloads)} external payloads")
    if version != pinned:
        raise Fail(
            f"kit version drift: manifest says {version}, kit.toml pins {pinned}.\n"
            f"       Microsoft has refreshed the download behind fwlink "
            f"{cfg['kit']['fwlink']}.\n"
            f"       Update [kit].version (and re-verify the MSI selection) to adopt it."
        )

    # 3. Which MSIs do we want?
    wanted = list(cfg["msi"]["required"])
    if not args.no_optional:
        wanted += list(cfg["msi"].get("optional", []))

    missing = [n for n in wanted if n not in payloads]
    if missing:
        raise Fail("these MSIs are not in the manifest (kit layout changed?):\n       "
                   + "\n       ".join(missing))

    # 4. Fetch the MSIs first; their Media tables tell us which cabs to fetch.
    if args.plan:
        total = sum(payloads[n].size for n in wanted)
        print(f"{len(wanted)} MSIs, {human(total)} (external cabs resolved after download)")
        for n in wanted:
            print(f"  {human(payloads[n].size):>10}  {n}")
        print("\nrun without --plan to download; cab totals are only known "
              "once the MSIs are on disk")
        return 0

    for name in wanted:
        pay = payloads[name]
        dest = installers / name
        if verify(dest, pay) is None:
            eprint(f"  have {name}")
            continue
        eprint(f"  get  {name} ({human(pay.size)})")
        download(payload_url(base, name), dest, pay)

    # 5. Each MSI's external cabs.
    cabs: dict[str, str] = {}   # cab name -> owning msi, for diagnostics
    for name in wanted:
        for cab in msi_external_cabs(installers / name):
            cabs.setdefault(cab, name)

    unknown = [c for c in cabs if c not in payloads]
    if unknown:
        raise Fail("MSIs reference cabs absent from the manifest:\n       "
                   + "\n       ".join(f"{c} (from {cabs[c]})" for c in unknown))

    cab_bytes = sum(payloads[c].size for c in cabs)
    eprint(f"{len(cabs)} external cabs, {human(cab_bytes)}")
    for cab, owner in sorted(cabs.items()):
        pay = payloads[cab]
        dest = installers / cab
        if verify(dest, pay) is None:
            eprint(f"  have {cab}")
            continue
        eprint(f"  get  {cab} ({human(pay.size)}, for {owner})")
        download(payload_url(base, cab), dest, pay)

    # 6. Record exactly what we fetched, so a test run can name its kit.
    files = wanted + sorted(cabs)
    record = {
        "kit_version": version,
        "fwlink": cfg["kit"]["fwlink"],
        "cdn_base": base,
        "payloads": {n: {"size": payloads[n].size, "sha1": payloads[n].sha1}
                     for n in files},
    }
    (kit_dir / "fetch-manifest.json").write_text(json.dumps(record, indent=2) + "\n")

    total = sum(payloads[n].size for n in files)
    eprint(f"\nfetched {len(files)} payloads, {human(total)} into {installers}")
    eprint("next: ./scripts/extract_kit.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
