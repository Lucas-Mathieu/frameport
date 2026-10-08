"""Find Quest games on disk.

Accepted layouts:
  <folder>/<something>.apk [+ <folder>/<package>/ (OBB or raw asset data)]   e.g. common downloader layouts
  <folder>/<package>.apk + <folder>/obb/                                       FramePort/PATCHED output layout
  <folder>/x.apk + <folder>/obb/<package>/ or Android/obb/<package>/          backups with an obb folder
  <game>/apk/x.apk + <game>/obb/<package>/                                     SideQuest-style backups (one game)
  a single .apk file
(find_data_dir has the details.)
"""
from __future__ import annotations

import re
import zipfile
from pathlib import Path

from ..core.models import SourceGame

# a release tag at the end of a download folder name: " -TAG" or " -TAG v76" (no space after the dash)
TAG = re.compile(r"\s+-(?=[A-Za-z])[A-Za-z0-9]{2,12}(?:\s+[A-Za-z]?\d{1,4})?\s*$")


def display_name(folder_name: str) -> str:
    """'PowerWash Simulator VR v3055+2.5.0 -TAG v76' -> 'PowerWash Simulator VR v3055+2.5.0'."""
    return re.sub(r'[<>:"/\\|?*]', "_", TAG.sub("", folder_name).strip())


def _package_of(apk: Path) -> str | None:
    try:
        from pyaxmlparser import APK

        return APK(str(apk)).package
    except Exception:
        return None


def _is_apk(path: Path) -> bool:
    try:
        with zipfile.ZipFile(path) as z:
            return "AndroidManifest.xml" in z.namelist()
    except (zipfile.BadZipFile, OSError):
        return False


OBB_FOLDERS = ("obb", "obbs")


def _subdirs(d: Path) -> list[Path]:
    try:
        return sorted(p for p in d.iterdir() if p.is_dir() and not p.name.startswith(("_", ".")))
    except OSError:
        return []


def _child(d: Path, name: str) -> Path | None:
    """d/name, matching the name's case loosely (backups come from Windows: 'OBB', a lower-case package folder)."""
    exact = d / name
    if exact.is_dir():
        return exact
    return next((p for p in _subdirs(d) if p.name.lower() == name.lower()), None)


def _nonempty(d: Path | None) -> bool:
    try:
        return bool(d) and d.is_dir() and any(d.iterdir())
    except OSError:
        return False


def _has_obb(d: Path) -> bool:
    try:
        return any(p.suffix.lower() == ".obb" and p.is_file() for p in d.iterdir())
    except OSError:
        return False


def _package_dir_below(d: Path, pkg: str, depth: int, skip: Path | None = None, other_games: bool = False) \
        -> Path | None:
    """A folder named like the package holding .obb files, at most `depth` levels below d. other_games: skip
    folders with APKs of their own (a neighbouring game folder, e.g. another version of the same game)."""
    for sub in _subdirs(d):
        if skip is not None and sub == skip:
            continue
        if sub.name.lower() == pkg.lower():
            if _has_obb(sub):
                return sub
            continue
        if depth > 1 and not (other_games and any(sub.glob("*.apk"))):
            found = _package_dir_below(sub, pkg, depth - 1)
            if found:
                return found
    return None


def find_data_dir(apk_dir: Path, pkg: str | None) -> Path | None:
    """The folder whose contents go to Android/obb/<package>/ (so the folder holding the .obb files themselves):
      <apk dir>/<package>/                       downloader layouts
      <apk dir>/obb/<package>/ or <apk dir>/obb/  FramePort/PATCHED output, backups with an obb folder
      a <package> folder with .obb files up to 3 levels below the APK's folder (e.g. Android/obb/<package>/) or 2
      below its parent (SideQuest-style backups: <game>/apk/x.apk + <game>/obb/<package>/; the parent's folders
      with APKs of their own are other games and are skipped)."""
    own = _child(apk_dir, pkg) if pkg else None
    if _nonempty(own):
        return own
    for obb in (_child(apk_dir, name) for name in OBB_FOLDERS):
        if obb is None:
            continue
        inner = _child(obb, pkg) if pkg else None
        if _nonempty(inner):
            return inner
        if _nonempty(obb):
            return obb
    if not pkg:
        return None
    found = _package_dir_below(apk_dir, pkg, 3)
    if found is None and apk_dir.parent != apk_dir:
        found = _package_dir_below(apk_dir.parent, pkg, 2, skip=apk_dir, other_games=True)
    return found


# a folder that only holds the APK inside a game folder (<game>/apk/x.apk + <game>/obb/…): the game is the parent
APK_FOLDERS = ("apk", "apks")


def from_path(path: Path) -> SourceGame | None:
    path = Path(path)
    if path.is_file() and path.suffix.lower() == ".apk":
        pkg = _package_of(path)
        data = find_data_dir(path.parent, pkg) if pkg else None
        return SourceGame(display_name(path.stem), path, data, path.parent)
    if not path.is_dir():
        return None
    apks = sorted(p for p in path.glob("*.apk") if _is_apk(p))
    if not apks:
        return None
    primary = [a for a in apks if ".alt-" not in a.name]
    apk = primary[0] if primary else apks[0]
    pkg = _package_of(apk)
    data = find_data_dir(path, pkg)
    game_dir = path.parent if path.name.lower() in APK_FOLDERS else path
    return SourceGame(display_name(game_dir.name), apk, data, game_dir, [a for a in apks if a != apk])


def _no_quest_games_below(d: Path) -> bool:
    """Folders not worth searching for APKs: a PC program (exe/dll files), a git checkout, a Python environment."""
    try:
        names = [p.name.lower() for p in d.iterdir()]
    except OSError:
        return True
    return any(n.endswith((".exe", ".dll")) for n in names) or ".git" in names or "pyvenv.cfg" in names


def _other_folders(d: Path, game: SourceGame) -> list[Path]:
    """Subfolders of a folder with an APK that aren't that game's own data (OBB) folder."""
    try:
        subs = [p for p in d.iterdir() if p.is_dir() and not p.name.startswith(("_", "."))]
    except OSError:
        return []
    own = {game.data_dir.resolve()} if game.data_dir else set()
    own |= set(game.data_dir.resolve().parents) if game.data_dir else set()  # e.g. Android/ of Android/obb/<pkg>
    return [p for p in subs if p.resolve() not in own and p.name.lower() not in (*OBB_FOLDERS, "android")]


def scan(root: Path, depth: int = 5) -> list[SourceGame]:
    """Find games under root, up to `depth` folder levels down (e.g. a download manager's
    "<library>/data/downloads/<game>/game.apk"). A folder with an APK and nothing but that game's data folder is one
    game; a folder that also has other folders is a collection: its loose APKs are games and its folders are searched
    (a "VR" folder with a stray APK next to the game folders used to count as one game). PC program folders and code
    checkouts aren't searched."""
    found: list[SourceGame] = []

    def walk(d: Path, level: int) -> None:
        game = from_path(d)
        if game and (level >= depth or not _other_folders(d, game)):
            found.append(game)
            return
        if game:  # a collection with loose APKs: each is a game of its own
            found.extend(g for g in (from_path(p) for p in sorted(d.glob("*.apk")) if _is_apk(p)) if g)
        if level >= depth:
            return
        try:
            children = sorted(p for p in d.iterdir() if p.is_dir() and not p.name.startswith(("_", ".")))
        except OSError:
            return
        for child in children:
            if game and game.data_dir and child.resolve() == game.data_dir.resolve():
                continue
            if from_path(child) or not _no_quest_games_below(child):
                walk(child, level + 1)

    root = Path(root)
    if root.is_file():
        g = from_path(root)
        return [g] if g else []
    walk(root, 0)
    return found
