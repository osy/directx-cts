"""Model an unmodified Wine installation's exported symbol surface.

Answers "will this PE load?" statically, which matters because the failure mode is
otherwise a bare loader error at run time.  Apiset names must be resolved the way Wine
does or the answer is wrong: they are redirected by apisetschema.dll, and only the part
before the *last* hyphen is hashed, so api-ms-win-dx-d3dkmt-l1-1-6 resolves through the
l1-1-0 entry.  Apiset resolution also wins over the file search, so a real DLL of that
name in the application directory is never consulted.
"""

from __future__ import annotations

import os
import struct
from functools import cached_property
from pathlib import Path

import pe as pemod

# Where a Wine installation keeps its builtin PE DLLs, one directory per architecture.
# WINE_DLL_DIR overrides the search for an installation none of these cover.
WINE_ROOTS = [
    Path("/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine"),
    Path("/usr/local/lib/wine"),
    Path("/usr/lib/wine"),
    Path("/opt/homebrew/lib/wine"),
]


class WinePrefix:
    """Exports of the builtin DLLs in one Wine <arch>-windows directory."""

    def __init__(self, dll_dir: Path):
        self.dll_dir = Path(dll_dir)
        if not self.dll_dir.is_dir():
            raise FileNotFoundError(self.dll_dir)

    @classmethod
    def find(cls, arch: str = "x64") -> "WinePrefix":
        sub = {"x64": "x86_64-windows", "x86": "i386-windows",
               "arm64": "aarch64-windows"}[arch]
        override = os.environ.get("WINE_DLL_DIR")
        roots = [Path(override)] if override else WINE_ROOTS
        for root in roots:
            if (root / sub).is_dir():
                return cls(root / sub)
        raise FileNotFoundError(
            f"no Wine {sub} directory under any of "
            + ", ".join(str(r) for r in roots)
            + "; set WINE_DLL_DIR")

    @cached_property
    def modules(self) -> dict[str, Path]:
        return {p.name.lower(): p for p in self.dll_dir.glob("*.dll")}

    @cached_property
    def apisets(self) -> dict[str, str]:
        """{hashed apiset name (no version suffix): host dll}."""
        schema = self.dll_dir / "apisetschema.dll"
        if not schema.is_file():
            return {}
        img = pemod.PEFile(schema)
        section = next((s for s in img.sections if s[0] == ".apiset"), None)
        if section is None:
            return {}
        _name, _va, _vsz, raddr, rsize = section
        blob = img.data[raddr:raddr + rsize]

        version, _size, _flags, count, entry_off, *_ = struct.unpack_from("<7I", blob)
        if version != 6:
            return {}

        out: dict[str, str] = {}
        for i in range(count):
            off = entry_off + i * 24
            (_f, name_off, _name_len, hashed_len,
             value_off, value_count) = struct.unpack_from("<6I", blob, off)
            key = blob[name_off:name_off + hashed_len].decode("utf-16-le").lower()
            if value_count:
                (_vf, _vn_off, _vn_len,
                 host_off, host_len) = struct.unpack_from("<5I", blob, value_off)
                host = blob[host_off:host_off + host_len].decode("utf-16-le").lower()
                out[key] = host
        return out

    def resolve(self, dll: str) -> str | None:
        """Map an imported module name to the Wine DLL that actually serves it."""
        name = dll.lower()
        if not name.startswith(("api-ms-", "ext-ms-")):
            return name if name in self.modules else None
        stem = name[:-4] if name.endswith(".dll") else name
        host = self.apisets.get(stem.rsplit("-", 1)[0])
        return host if host and host in self.modules else None

    def exports_of(self, module: str) -> set[str]:
        cache = self.__dict__.setdefault("_export_cache", {})
        key = module.lower()
        if key not in cache:
            path = self.modules.get(key)
            names: set[str] = set()
            if path is not None:
                for exp in pemod.PEFile(path).exports():
                    names.add(exp.name or f"#{exp.ordinal}")
                    names.add(f"#{exp.ordinal}")
            cache[key] = names
        return cache[key]

    def check(self, image: Path, include_delay: bool = False) -> list[str]:
        """Reasons `image` would fail to load, or [] if it resolves.

        Delay-load imports are excluded by default: they bind on first call, so an
        unresolvable one is a latent failure on that code path, not a load failure.
        """
        problems: list[str] = []
        for imp in pemod.PEFile(image).all_imports():
            if imp.delay and not include_delay:
                continue
            host = self.resolve(imp.dll)
            if host is None:
                kind = "delay-load " if imp.delay else ""
                problems.append(f"{kind}module not in Wine: {imp.dll}")
                continue
            available = self.exports_of(host)
            missing = [fn for fn in imp.functions if fn not in available]
            if missing:
                where = f"{imp.dll} -> {host}" if host != imp.dll.lower() else host
                kind = "delay-load " if imp.delay else ""
                problems.append(
                    f"{kind}{where} lacks: " + ", ".join(sorted(missing)[:12])
                    + (f" (+{len(missing) - 12} more)" if len(missing) > 12 else ""))
        return problems
