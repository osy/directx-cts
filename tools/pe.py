"""Minimal PE reader/writer for the bits directx-cts needs.

Used to
  * generate the WTTLog proxy's .def from what the kit binaries actually import,
  * drive the staging-time, length-preserving import-name rewrite,
  * inventory the kit (which binary needs which DLL, and from where).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

MACHINE = {0x014C: "x86", 0x8664: "x64", 0xAA64: "arm64", 0x01C4: "arm"}

DIR_EXPORT, DIR_IMPORT, DIR_DELAY = 0, 1, 13


class NotPE(ValueError):
    pass


@dataclass
class Import:
    dll: str
    functions: list[str] = field(default_factory=list)  # "Name" or "#ordinal"
    name_offset: int = 0     # file offset of the DLL name string
    delay: bool = False


@dataclass
class Export:
    name: str
    ordinal: int
    forwarder: str | None = None


class PEFile:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        if d[:2] != b"MZ":
            raise NotPE(f"{self.path.name}: not an MZ image")
        self.pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.pe:self.pe + 4] != b"PE\0\0":
            raise NotPE(f"{self.path.name}: no PE signature")

        self.machine = struct.unpack_from("<H", d, self.pe + 4)[0]
        nsec = struct.unpack_from("<H", d, self.pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, self.pe + 20)[0]
        opt = self.pe + 24
        self.magic = struct.unpack_from("<H", d, opt)[0]
        self.pe32plus = self.magic == 0x20B
        self.image_base = struct.unpack_from(
            "<Q" if self.pe32plus else "<I", d, opt + 24)[0]

        ddir = opt + (112 if self.pe32plus else 96)
        ndir = struct.unpack_from("<I", d, opt + (108 if self.pe32plus else 92))[0]
        self.dirs = [struct.unpack_from("<II", d, ddir + 8 * i) for i in range(ndir)]

        self.sections = []
        sec0 = self.pe + 24 + opt_size
        for i in range(nsec):
            o = sec0 + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))

    # -- helpers ----------------------------------------------------------

    @property
    def arch(self) -> str:
        return MACHINE.get(self.machine, f"0x{self.machine:04x}")

    def rva_to_off(self, rva: int) -> int | None:
        for _name, vaddr, vsize, raddr, rsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                off = raddr + (rva - vaddr)
                return off if off < len(self.data) else None
        return None

    def cstr(self, off: int) -> str:
        end = self.data.find(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def _dir(self, index: int) -> tuple[int, int]:
        return self.dirs[index] if index < len(self.dirs) else (0, 0)

    # -- imports ----------------------------------------------------------

    def _thunks(self, rva: int) -> list[str]:
        """Walk an import lookup table, returning names or #ordinals."""
        off = self.rva_to_off(rva)
        if off is None:
            return []
        step = 8 if self.pe32plus else 4
        fmt = "<Q" if self.pe32plus else "<I"
        ordinal_flag = (1 << 63) if self.pe32plus else (1 << 31)
        out: list[str] = []
        while off + step <= len(self.data):
            value = struct.unpack_from(fmt, self.data, off)[0]
            if value == 0:
                break
            if value & ordinal_flag:
                out.append(f"#{value & 0xFFFF}")
            else:
                hint = self.rva_to_off(value & 0x7FFFFFFF)
                out.append(self.cstr(hint + 2) if hint is not None else "?")
            off += step
        return out

    def imports(self) -> list[Import]:
        out: list[Import] = []
        rva, _size = self._dir(DIR_IMPORT)
        off = self.rva_to_off(rva) if rva else None
        while off is not None and off + 20 <= len(self.data):
            chunk = self.data[off:off + 20]
            if chunk == b"\0" * 20:
                break
            oft, _ts, _fc, name_rva, ft = struct.unpack("<IIIII", chunk)
            noff = self.rva_to_off(name_rva)
            if noff is None:
                break
            out.append(Import(self.cstr(noff), self._thunks(oft or ft), noff))
            off += 20
        return out

    def delay_imports(self) -> list[Import]:
        out: list[Import] = []
        rva, _size = self._dir(DIR_DELAY)
        off = self.rva_to_off(rva) if rva else None
        while off is not None and off + 32 <= len(self.data):
            chunk = self.data[off:off + 32]
            if chunk == b"\0" * 32:
                break
            attrs, name_rva, _mod, _iat, int_rva = struct.unpack_from("<IIIII", chunk)
            # Attributes bit 0 clear means the old format, where these are VAs.
            if not (attrs & 1):
                name_rva -= self.image_base
                int_rva -= self.image_base
            noff = self.rva_to_off(name_rva)
            if noff is None:
                break
            out.append(Import(self.cstr(noff), self._thunks(int_rva), noff,
                              delay=True))
            off += 32
        return out

    def all_imports(self) -> list[Import]:
        return self.imports() + self.delay_imports()

    # -- exports ----------------------------------------------------------

    def exports(self) -> list[Export]:
        rva, size = self._dir(DIR_EXPORT)
        base_off = self.rva_to_off(rva) if rva else None
        if base_off is None:
            return []
        d = self.data
        ordinal_base = struct.unpack_from("<I", d, base_off + 0x10)[0]
        nfuncs = struct.unpack_from("<I", d, base_off + 0x14)[0]
        nnames = struct.unpack_from("<I", d, base_off + 0x18)[0]
        funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", d, base_off + 0x1C)

        foff = self.rva_to_off(funcs_rva)
        noff = self.rva_to_off(names_rva)
        ooff = self.rva_to_off(ords_rva)

        named: dict[int, str] = {}
        if noff is not None and ooff is not None:
            for i in range(nnames):
                nr = struct.unpack_from("<I", d, noff + 4 * i)[0]
                so = self.rva_to_off(nr)
                idx = struct.unpack_from("<H", d, ooff + 2 * i)[0]
                if so is not None:
                    named[idx] = self.cstr(so)

        out: list[Export] = []
        if foff is None:
            return out
        for i in range(nfuncs):
            frva = struct.unpack_from("<I", d, foff + 4 * i)[0]
            if frva == 0:
                continue  # hole in the ordinal space
            fwd = None
            if rva <= frva < rva + size:          # points inside the export dir
                so = self.rva_to_off(frva)
                if so is not None:
                    fwd = self.cstr(so)
            out.append(Export(named.get(i, ""), ordinal_base + i, fwd))
        return out


def rewrite_module_string(path: Path, old: str, new: str) -> int:
    """Rewrite NUL-terminated occurrences of a module name in the image data.

    The HLK framework resolves d3d11/d3d12/dxgi with LoadLibrary on a string literal
    rather than through its import table, so retargeting the import alone leaves
    device creation going to the stock module.  Both encodings and any spelling are
    matched: DxgTaef's device cache, for one, loads L"DXGI.dll" wide and upper-case
    and then GetProcAddress's CreateDXGIFactory2 out of it, which an ASCII
    lower-case search does not see.  Length-preserving for the same reason as the
    import rewrite.
    """
    if len(old) != len(new):
        raise ValueError(
            f"string rewrite must preserve length: {old!r} ({len(old)}) -> "
            f"{new!r} ({len(new)})")

    data = bytearray(path.read_bytes())
    count = 0
    for encoding, terminator in (("ascii", b"\0"), ("utf-16-le", b"\0\0")):
        needle = (old.encode(encoding) + terminator).lower()
        replacement = new.encode(encoding) + terminator
        lowered = bytes(data).lower()
        start = 0
        while (idx := lowered.find(needle, start)) != -1:
            data[idx:idx + len(replacement)] = replacement
            start = idx + len(replacement)
            count += 1
    if count:
        path.write_bytes(bytes(data))
    return count


def rewrite_import_name(path: Path, old: str, new: str) -> int:
    """Rewrite an imported DLL name in place. Returns how many descriptors changed.

    Length-preserving by construction: PE import names are NUL-terminated strings
    referenced by RVA, so a same-length replacement needs no relocation. Anything
    else would mean rebuilding the import directory, which we deliberately do not do.
    """
    if len(old) != len(new):
        raise ValueError(
            f"import rewrite must preserve length: {old!r} ({len(old)}) -> "
            f"{new!r} ({len(new)})")

    pe = PEFile(path)
    offsets = [imp.name_offset for imp in pe.all_imports()
               if imp.dll.lower() == old.lower()]
    if not offsets:
        return 0

    data = bytearray(pe.data)
    for off in offsets:
        data[off:off + len(new)] = new.encode("ascii")
    path.write_bytes(bytes(data))
    return len(offsets)
