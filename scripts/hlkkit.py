"""Shared helpers for the HLK acquisition scripts.

Everything here is stdlib-only so the scripts run on a stock python3 (3.11+ for
tomllib) on macOS, Linux or Windows.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tomllib
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_KIT_DIR = PROJECT_ROOT / "kit"


class Fail(SystemExit):
    """Abort with a message the user can act on."""

    def __init__(self, msg: str) -> None:
        super().__init__(f"error: {msg}")


def load_config(path: Path | None = None) -> dict:
    path = path or PROJECT_ROOT / "kit.toml"
    if not path.is_file():
        raise Fail(f"no kit.toml at {path}")
    with path.open("rb") as fh:
        return tomllib.load(fh)


def need_tool(name: str, hint: str) -> str:
    exe = shutil.which(name)
    if not exe:
        raise Fail(f"{name!r} not found on PATH. {hint}")
    return exe


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


# --- Burn manifest ---------------------------------------------------------

@dataclass(frozen=True)
class Payload:
    name: str      # basename as served under <base>/Installers/
    size: int
    sha1: str      # uppercase hex, from the manifest


def _localname(elem: ET.Element) -> str:
    return elem.tag.rsplit("}", 1)[-1]


def parse_manifest(xml_bytes: bytes) -> tuple[str, dict[str, Payload]]:
    """Return (bundle version, {payload name: Payload}) from a Burn manifest."""
    root = ET.fromstring(xml_bytes.decode("utf-8", "replace"))

    version = ""
    for elem in root.iter():
        if _localname(elem) == "Registration":
            version = elem.get("Version", "")
            break

    payloads: dict[str, Payload] = {}
    for elem in root.iter():
        if _localname(elem) != "Payload" or elem.get("Packaging") != "external":
            continue
        name = elem.get("FilePath", "").split("\\")[-1]
        if not name:
            continue
        payloads[name] = Payload(name, int(elem.get("FileSize", "0")),
                                 (elem.get("Hash") or "").upper())
    return version, payloads


def extract_burn_manifest(setup_exe: Path, workdir: Path) -> bytes:
    """Pull BurnManifest.xml out of a WiX Burn bundle.

    The bundle carries an MSZip UX cabinet whose first entry is named `0` and *is*
    the manifest; the remaining `u0`..`uN` entries are the bootstrapper UI payloads.
    """
    sevenzip = need_tool("7z", "Install p7zip (macOS: brew install p7zip).")
    out = workdir / "ux"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    proc = run([sevenzip, "x", "-y", f"-o{out}", str(setup_exe)])
    if proc.returncode != 0:
        raise Fail(f"7z could not unpack {setup_exe.name}:\n{proc.stderr.strip()}")

    manifest = out / "0"
    if not manifest.is_file():
        # Be forgiving if a future bundle names it properly.
        alt = next((p for p in out.rglob("BurnManifest.xml")), None)
        if alt is None:
            raise Fail(f"no Burn manifest inside {setup_exe.name} "
                       f"(looked for '0' and BurnManifest.xml under {out})")
        manifest = alt
    return manifest.read_bytes()


# --- downloading -----------------------------------------------------------

def resolve_redirect(url: str) -> str:
    """Follow redirects and return the final URL, without fetching the body."""
    req = urllib.request.Request(url, method="HEAD",
                                 headers={"User-Agent": "directx-cts"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.geturl()


def sha1_of(path: Path, chunk: int = 1 << 20) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as fh:
        while block := fh.read(chunk):
            digest.update(block)
    return digest.hexdigest().upper()


def verify(path: Path, expect: Payload) -> str | None:
    """Return None if the file matches the manifest, else a reason string."""
    if not path.is_file():
        return "missing"
    actual = path.stat().st_size
    if actual != expect.size:
        return f"size {actual} != {expect.size}"
    if expect.sha1:
        got = sha1_of(path)
        if got != expect.sha1:
            return f"sha1 {got} != {expect.sha1}"
    return None


def download(url: str, dest: Path, expect: Payload | None = None) -> None:
    """Fetch url to dest, verifying against the manifest entry when given."""
    if expect is not None and verify(dest, expect) is None:
        return  # already have a good copy

    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "directx-cts"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp, tmp.open("wb") as out:
            shutil.copyfileobj(resp, out, 1 << 20)
    except Exception as exc:  # noqa: BLE001 - surface the URL with the error
        tmp.unlink(missing_ok=True)
        raise Fail(f"download failed for {url}: {exc}") from exc

    if expect is not None:
        why = verify(tmp, expect)
        if why is not None:
            tmp.unlink(missing_ok=True)
            raise Fail(f"{dest.name}: {why}")
    tmp.replace(dest)


def payload_url(base: str, name: str) -> str:
    return f"{base}/Installers/{urllib.parse.quote(name)}"


# --- MSI ------------------------------------------------------------------

def msi_external_cabs(msi: Path) -> list[str]:
    """Cab files an MSI needs alongside it.

    The Media table's Cabinet column names an embedded stream when it starts with
    '#'; those travel inside the MSI and must not be downloaded separately.
    """
    msiinfo = need_tool("msiinfo", "Install msitools (macOS: brew install msitools).")
    proc = run([msiinfo, "export", str(msi), "Media"])
    if proc.returncode != 0:
        raise Fail(f"msiinfo could not read {msi.name}:\n{proc.stderr.strip()}")

    cabs: list[str] = []
    for line in proc.stdout.splitlines()[3:]:  # skip column names/types/key rows
        fields = line.split("\t")
        if len(fields) < 4:
            continue
        cab = fields[3].strip()
        if cab and not cab.startswith("#"):
            cabs.append(cab)
    return cabs


def eprint(*args) -> None:
    print(*args, file=sys.stderr, flush=True)
