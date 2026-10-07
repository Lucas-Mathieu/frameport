"""What to do with files dropped on the Library window (no Flet, tested directly)."""
from __future__ import annotations

import os
from pathlib import Path

MAX_LOOK = 5000  # entries looked at in a dropped folder before deciding


def _folder_kind(folder: Path) -> str:
    """"folder" (APKs or a Windows game: scanned like "Scan a folder") or "linux" (Linux programs only)."""
    from ..analysis import linux

    elf = False
    seen = 0
    for root, dirs, files in os.walk(folder):
        if len(Path(root).relative_to(folder).parts) >= 3:
            dirs[:] = []
        for name in files:
            seen += 1
            low = name.lower()
            if low.endswith((".apk", ".exe")):
                return "folder"
            if not elf and ("." not in name or low.endswith(".appimage")):
                elf = linux.elf_machine(Path(root) / name) is not None or linux.is_appimage(Path(root) / name)
            if seen > MAX_LOOK:
                return "linux" if elf else "folder"
    return "linux" if elf else "folder"


def route(paths: list[str]) -> list[tuple[str, str]]:
    """[(kind, path)] with kind apk | linux | exe | manifest | folder | unknown."""
    from ..analysis import linux

    out = []
    for raw in paths:
        p = Path(raw)
        low = p.name.lower()
        if p.is_dir():
            out.append((_folder_kind(p), raw))
        elif low.endswith(".apk"):
            out.append(("apk", raw))
        elif low.endswith(".exe"):
            out.append(("exe", raw))
        elif low.endswith(".json"):
            out.append(("manifest", raw))
        elif p.is_file() and linux.looks_like_linux_app(p):
            out.append(("linux", raw))
        else:
            out.append(("unknown", raw))
    return out
