#!/usr/bin/env python3
"""Extract the fetched HLK MSIs into one normalised kit tree.

Every graphics MSI installs under "Windows Kits/10/...", so they are all extracted
into a single root and that prefix is flattened away, giving the layout the HLK
controller would have produced under C:\\Program Files (x86)\\Windows Kits\\10.

    ./scripts/extract_kit.py            # kit/Installers -> kit/WindowsKits10
    ./scripts/extract_kit.py --force    # re-extract over an existing tree

Finishes with the [verify] closure check from kit.toml, so an incomplete MSI
selection fails here with the missing paths named rather than at run time.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from hlkkit import DEFAULT_KIT_DIR, Fail, eprint, load_config, need_tool, run  # noqa: E402

KIT_ROOT_NAME = "WindowsKits10"


def kit_root_in(staging: Path) -> Path:
    """Find the "Windows Kits/10" directory msiextract produced.

    Its depth is not fixed: the graphics MSIs land it under "Program Files/", others
    may not, so search rather than assume.  Falls back to the staging root for MSIs
    that install somewhere else entirely.
    """
    for candidate in sorted(staging.rglob("10"), key=lambda p: len(p.parts)):
        if candidate.is_dir() and candidate.parent.name == "Windows Kits":
            return candidate
    return staging


def flatten(staging: Path, dest: Path) -> None:
    """Merge the extracted kit tree into dest, dropping the install-path prefix."""
    src = kit_root_in(staging)
    for item in src.iterdir():
        merge(item, dest / item.name)


def merge(src: Path, dst: Path) -> None:
    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            merge(child, dst / child.name)
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists():
        shutil.move(str(src), str(dst))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kit-dir", type=Path, default=DEFAULT_KIT_DIR)
    ap.add_argument("--force", action="store_true",
                    help="delete an existing extracted tree first")
    ap.add_argument("--config", type=Path, default=None)
    args = ap.parse_args()

    cfg = load_config(args.config)
    kit_dir: Path = args.kit_dir
    installers = kit_dir / "Installers"
    root = kit_dir / KIT_ROOT_NAME

    if not installers.is_dir():
        raise Fail(f"no {installers}; run ./scripts/fetch_kit.py first")

    if root.exists():
        if not args.force:
            raise Fail(f"{root} already exists; pass --force to re-extract")
        shutil.rmtree(root)
    root.mkdir(parents=True)

    msiextract = need_tool(
        "msiextract", "Install msitools (macOS: brew install msitools).")

    wanted = list(cfg["msi"]["required"]) + list(cfg["msi"].get("optional", []))
    present = [n for n in wanted if (installers / n).is_file()]
    absent = [n for n in wanted if n not in present]
    if not present:
        raise Fail(f"no MSIs found in {installers}; run ./scripts/fetch_kit.py")
    for name in absent:
        eprint(f"  skip {name} (not downloaded)")

    staging = kit_dir / ".work" / "extract"
    for name in present:
        eprint(f"  extract {name}")
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        # msiextract resolves cabs relative to the cwd, so run it beside the MSI.
        proc = run([msiextract, "-C", str(staging.resolve()), name],
                   cwd=str(installers))
        if proc.returncode != 0:
            err = proc.stderr.strip()
            if ".cab" in err and "No such file" in err:
                cabs = sorted(set(re.findall(r"[^\s/\\]+\.cab", err)))
                raise Fail(
                    f"{name} needs cabs that are not in {installers}:\n       "
                    + "\n       ".join(cabs)
                    + "\n\n       Run ./scripts/fetch_kit.py to download them."
                )
            raise Fail(f"msiextract failed on {name}:\n{err}")
        flatten(staging, root)
    shutil.rmtree(kit_dir / ".work", ignore_errors=True)

    # Closure check: did the selection actually produce everything we depend on?
    missing = [p for p in cfg.get("verify", {}).get("required_files", [])
               if not (root / p).exists()]
    if missing:
        raise Fail(
            "extraction is incomplete - these expected files are absent:\n       "
            + "\n       ".join(missing)
            + "\n\n       The [msi] selection in kit.toml no longer covers everything.\n"
              "       Find the owning package with:\n"
              "         for m in kit/Installers/*.msi; do\n"
              "           msiinfo export \"$m\" File | grep -qi <filename> && echo \"$m\"\n"
              "         done"
        )

    files = sum(1 for _ in root.rglob("*") if _.is_file())
    eprint(f"\nextracted {len(present)} MSIs -> {root} ({files} files)")
    eprint(f"verified {len(cfg.get('verify', {}).get('required_files', []))} required paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
