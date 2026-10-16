"""Library folders (GitHub discussion #351): the folders "Rescan folders" scans again (library setting `scan.roots`).

Every "Scan a folder…" remembers its folder; Settings → Library folders and `frameport folders` list them with their
status and let the user add or remove one. Removing a folder only stops it being scanned: the games already in the
library stay. No Flet here (the CLI uses it too)."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from . import library

SETTING = "scan.roots"


@dataclass
class FolderStatus:
    path: str
    exists: bool
    games: int  # library games whose files (APK, data or game folder) are in it


def roots() -> list[str]:
    """The remembered folders, in the order they were added (missing ones too)."""
    return [str(r) for r in (library.setting(SETTING) or [])]


def _same(a: str, b: str) -> bool:
    if a == b:
        return True
    try:
        return Path(a).resolve() == Path(b).resolve()
    except OSError:
        return False


def remember(path: str | Path) -> list[str]:
    """Add a folder to the list (a scanned one): moved to the end if it's there already. Returns the new list."""
    path = str(path)
    return library.update_setting(SETTING, lambda cur: [r for r in (cur or []) if not _same(str(r), path)] + [path],
                                  [])


def add(path: str | Path) -> list[str]:
    """Settings/CLI "Add folder…": like remember, but only an existing folder."""
    p = Path(path).expanduser()
    if not p.is_dir():
        raise ValueError(f"{p} isn't a folder")
    if any(_same(r, str(p)) for r in roots()):
        return roots()
    return remember(p)


def remove(path: str | Path) -> bool:
    """Stop scanning a folder (its games stay in the library). False if it wasn't in the list."""
    path = str(path)
    found = {"hit": False}

    def drop(cur):
        keep = [r for r in (cur or []) if not _same(str(r), path)]
        found["hit"] = len(keep) != len(cur or [])
        return keep
    library.update_setting(SETTING, drop, [])
    return found["hit"]


def game_paths(entry: dict) -> list[Path]:
    """Where a library game's files are (APK, data folder, game folder)."""
    out = []
    for key in ("apk", "data_dir", "game_dir"):
        if entry.get(key):
            out.append(Path(entry[key]))
    return out


def _inside(p: Path, folder: Path) -> bool:
    try:
        return p == folder or p.is_relative_to(folder)
    except (OSError, ValueError):
        return False


def games_in(folder: str | Path, games: list[dict] | None = None) -> list[dict]:
    """Library games whose files are in the folder (by path, no disk access)."""
    folder = Path(folder)
    games = library.games() if games is None else games
    return [g for g in games if g.get("kind") != "linux" and any(_inside(p, folder) for p in game_paths(g))]


def status() -> list[FolderStatus]:
    """Each remembered folder: whether it's there now and how many library games come from it. Touches the disk
    (exists): call it off the UI thread."""
    games = library.games()
    out = []
    for r in roots():
        try:
            exists = Path(r).is_dir()
        except OSError:
            exists = False
        out.append(FolderStatus(r, exists, len(games_in(r, games))))
    return out


def rescan_roots() -> list[str]:
    """Folders "Rescan folders" scans: the remembered ones that exist now. Before anything was remembered (libraries
    from older versions), the folders the library's games came from (one level up from each game folder). A folder
    inside another one is scanned with it."""
    saved = library.setting(SETTING)
    if saved is not None:
        found = [r for r in saved if Path(r).exists()]
    else:
        seen = set()
        for g in library.games():
            if g.get("kind") == "linux":  # added one by one (its origin may be e.g. Downloads)
                continue
            origin = g.get("origin") or (str(Path(g["apk"]).parent) if g.get("apk") else None)
            if origin and Path(origin).parent.exists():
                seen.add(str(Path(origin).parent))
        found = sorted(seen)
    return [r for r in found if not any(o != r and Path(r).is_relative_to(o) for o in found)]
