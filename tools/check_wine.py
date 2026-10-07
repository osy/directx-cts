#!/usr/bin/env python3
"""Report what stops HLK binaries loading under an unmodified Wine.

    ./tools/check_wine.py kit/WindowsKits10/.../d3d/conf            # a directory
    ./tools/check_wine.py --summary <dir>...                        # group by cause
    ./tools/check_wine.py build/x64/bin/DEVOBJ.dll                  # our own shims

Static only: imports are resolved against Wine's builtin exports, apiset redirection
included.  A clean report means the loader will not reject the image; it says nothing
about whether the test then passes.
"""

from __future__ import annotations

import argparse
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pe as pemod  # noqa: E402
from wineprefix import WinePrefix  # noqa: E402


def images(paths: list[Path]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        if p.is_dir():
            out += [q for q in sorted(p.rglob("*"))
                    if q.is_file() and q.suffix.lower() in (".exe", ".dll")]
        elif p.is_file():
            out.append(p)      # named explicitly: inspect it whatever it is called
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--arch", default="x64", choices=["x64", "x86", "arm64"])
    ap.add_argument("--delay-loads", action="store_true",
                    help="also report delay-load imports (latent, not load failures)")
    ap.add_argument("--summary", action="store_true",
                    help="group by missing symbol instead of listing per binary")
    ap.add_argument("--app-dir", type=Path, action="append", default=[],
                    help="directory whose DLLs sit beside the binaries at run time "
                         "(kit support dir, TAEF runtime, our shim output)")
    args = ap.parse_args()

    wine = WinePrefix.find(args.arch)
    # The application directory is searched before system32 for ordinary module
    # names, so DLLs staged beside the tests resolve there.  Apiset names are
    # exempt: those never reach the file search.
    for extra in args.app_dir:
        for dll in sorted(extra.rglob("*.dll")) if extra.is_dir() else [extra]:
            wine.modules.setdefault(dll.name.lower(), dll)

    targets = images(args.paths)
    blocked: dict[Path, list[str]] = {}
    for image in targets:
        try:
            problems = wine.check(image, include_delay=args.delay_loads)
        except pemod.NotPE:
            continue
        if problems:
            blocked[image] = problems

    if args.summary:
        causes: dict[str, set[str]] = collections.defaultdict(set)
        for image, problems in blocked.items():
            for why in problems:
                causes[why].add(image.name)
        for why, who in sorted(causes.items(), key=lambda kv: -len(kv[1])):
            print(f"{len(who):4d}  {why}")
            print(f"      {', '.join(sorted(who)[:6])}"
                  + (f", +{len(who) - 6} more" if len(who) > 6 else ""))
    else:
        for image, problems in sorted(blocked.items()):
            print(image.name)
            for why in problems:
                print(f"    {why}")

    print(f"\n{len(targets) - len(blocked)}/{len(targets)} images load; "
          f"{len(blocked)} blocked", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
