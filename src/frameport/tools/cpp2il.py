"""Cpp2IL (SamboyCoding/Cpp2IL, MIT): reads an IL2CPP Unity game's libil2cpp.so + global-metadata.dat and lists every
method with its address. FramePort needs it only for Unity games whose text fields it fixes (frame.unity_text_input),
so it is downloaded on first use into <user data>/tools/ (a self-contained executable per OS; no .NET install).

Its current releases are all pre-releases (the 2022.1 line is the one that reads Unity 6 / metadata v31), so the
newest release with an asset for this computer is used. Override: FRAMEPORT_CPP2IL=<path to the executable>.

Unity 6.5+ (metadata v107/v108) is only read by Cpp2IL's development branch so far (no release yet): when the
release says it can't read a game's metadata version, the development branch's CI build (nightly.link, the download
Cpp2IL's README points to) is fetched into tools/cpp2il-nightly/ and used for that game."""
from __future__ import annotations

import os
import platform
import re
import stat
import sys
import zipfile
from pathlib import Path

from ..core import cache
from ..core.paths import tools_dir

RELEASES = "https://api.github.com/repos/SamboyCoding/Cpp2IL/releases"
NIGHTLY = "https://nightly.link/SamboyCoding/Cpp2IL/workflows/dotnet-core/development/Cpp2IL-net9-{rid}.zip"
NIGHTLY_DIR = "cpp2il-nightly"
# what a release prints for metadata it can't read, e.g. "Unsupported metadata version ... We support 23-106, got 107"
_UNSUPPORTED = re.compile(r"unsupported metadata version|we support \d+-\d+", re.I)


def asset_suffix() -> str:
    """Release asset suffix for this computer, e.g. Linux, Linux-ARM64, OSX-ARM64, Windows.exe."""
    arm = platform.machine().lower() in ("arm64", "aarch64")
    if sys.platform == "win32":
        return "Windows-ARM64.exe" if arm else "Windows.exe"
    if sys.platform == "darwin":
        return "OSX-ARM64" if arm else "OSX"
    return "Linux-ARM64" if arm else "Linux"


def nightly_rid() -> str:
    """The development build's runtime id for this computer (there is no Windows ARM64 build: x64 runs emulated)."""
    arm = platform.machine().lower() in ("arm64", "aarch64")
    if sys.platform == "win32":
        return "win-x64"
    if sys.platform == "darwin":
        return "osx-arm64" if arm else "osx-x64"
    return "linux-arm64" if arm else "linux-x64"


def unsupported_metadata(error: str) -> bool:
    """Cpp2IL's message for a metadata version it can't read (a newer Unity than the release knows)."""
    return bool(_UNSUPPORTED.search(error or ""))


def _exe_name() -> str:
    return "Cpp2IL.exe" if sys.platform == "win32" else "Cpp2IL"


def pick_release(releases: list[dict], suffix: str) -> tuple[str, str] | None:
    """(version, download url) of the newest release that has an executable for this computer."""
    for rel in releases:
        for asset in rel.get("assets", []):
            if asset.get("name", "").endswith("-" + suffix):
                return rel.get("tag_name", "?"), asset["browser_download_url"]
    return None


def path() -> Path | None:
    if os.environ.get("FRAMEPORT_CPP2IL"):
        return Path(os.environ["FRAMEPORT_CPP2IL"])
    found = sorted(p for p in tools_dir().glob(f"cpp2il-*/{_exe_name()}")  # may hold other OSes' copies too
                   if p.parent.name != NIGHTLY_DIR)
    return found[-1] if found else None


def installed_version() -> str | None:
    p = path()
    return p.parent.name.removeprefix("cpp2il-") if p is not None and p.parent.name.startswith("cpp2il-") else None


def ensure(progress=None) -> Path:
    """The Cpp2IL executable, downloading it first if needed."""
    existing = path()
    if existing is not None and existing.exists():
        return existing
    releases = cache.cached_json("cpp2il-releases.json", RELEASES, max_age=7 * 86400, fallback=[]) or []
    picked = pick_release(releases, asset_suffix())
    if picked is None:
        raise RuntimeError("couldn't find a Cpp2IL download for this computer")
    version, url = picked
    exe = tools_dir() / f"cpp2il-{version}" / _exe_name()
    cache.download(url, exe, progress)
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return exe


def ensure_nightly(progress=None) -> Path:
    """The development branch's Cpp2IL (newer Unity metadata than any release), downloading it first if needed."""
    exe = tools_dir() / NIGHTLY_DIR / _exe_name()
    if exe.exists():
        return exe
    archive = exe.with_name("cpp2il.zip")
    cache.download(NIGHTLY.format(rid=nightly_rid()), archive, progress)
    with zipfile.ZipFile(archive) as z:
        member = next((n for n in z.namelist() if Path(n).name == _exe_name()), None)
        if member is None:
            raise RuntimeError("the Cpp2IL development build has no executable for this computer")
        tmp = exe.with_name(exe.name + ".part")
        tmp.write_bytes(z.read(member))
    archive.unlink(missing_ok=True)
    tmp.chmod(tmp.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    tmp.replace(exe)
    return exe
