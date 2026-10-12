"""File pickers without the desktop portal (GitHub #208).

Flet's Linux file picker asks the XDG desktop portal (org.freedesktop.portal.Desktop); window managers without one
(IceWM, bare X11 sessions) answer NameHasNoOwner and the pick raises. The GUI then falls back to zenity or kdialog
(run here, no Flet), and without either to a "type a path" box. Everything in this module is pure apart from `run`.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from collections.abc import Callable, Mapping
from pathlib import Path

TOOLS = ("zenity", "kdialog")
TIMEOUT = 3600  # a picker left open for an hour: give up


class PickerError(RuntimeError):
    pass


def portal_missing(exc: BaseException, platform: str) -> bool:
    """Whether a failed Flet pick means "no usable portal": on Linux every RuntimeError from the picker does (the
    message is D-Bus' own, e.g. NameHasNoOwner or ServiceUnknown); elsewhere it's a real error."""
    return platform.startswith("linux") and isinstance(exc, RuntimeError)


def choose_tool(env: Mapping[str, str] | None = None, which: Callable[[str], str | None] = shutil.which
                ) -> tuple[str, str] | None:
    """(name, path) of the picker program to use: kdialog first on KDE, else zenity, else whichever exists."""
    env = os.environ if env is None else env
    order = TOOLS[::-1] if "KDE" in env.get("XDG_CURRENT_DESKTOP", "").upper() else TOOLS
    for name in order:
        path = which(name)
        if path:
            return name, path
    return None


def argv(tool: str, program: str, *, directory: bool = False, multiple: bool = False, title: str | None = None,
         extensions: list[str] | None = None, start: str | None = None) -> list[str]:
    """The command line that asks for one folder, or one/several files (optionally only these extensions)."""
    start = start or str(Path.home())
    exts = [x.lstrip(".") for x in extensions or []]
    globs = " ".join(f"*.{x}" for x in exts)
    if tool == "zenity":
        cmd = [program, "--file-selection"]
        if title:
            cmd += ["--title", title]
        cmd += ["--filename", start.rstrip("/") + "/"]
        if directory:
            cmd.append("--directory")
        else:
            if multiple:
                cmd += ["--multiple", "--separator", "\n"]
            if exts:
                cmd += ["--file-filter", globs]
        return cmd
    if tool == "kdialog":
        cmd = [program]
        if title:
            cmd += ["--title", title]
        if directory:
            return cmd + ["--getexistingdirectory", start]
        cmd += ["--getopenfilename", start]
        if exts:
            cmd.append(globs)
        if multiple:
            cmd += ["--multiple", "--separate-output"]
        return cmd
    raise ValueError(f"unknown picker {tool!r}")


def parse(output: str) -> list[str]:
    """The picked paths (one per line; both tools print nothing when cancelled)."""
    return [line.strip() for line in output.splitlines() if line.strip()]


def run(cmd: list[str], runner: Callable = subprocess.run) -> list[str]:
    """Run a picker and return the picked paths ([] = cancelled). Exit 1 is both tools' "cancelled"."""
    try:
        r = runner(cmd, capture_output=True, text=True, timeout=TIMEOUT)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise PickerError(f"{Path(cmd[0]).name} didn't run: {exc}") from exc
    if r.returncode == 1:
        return []
    if r.returncode:
        raise PickerError((r.stderr or "").strip() or f"{Path(cmd[0]).name} failed ({r.returncode})")
    return parse(r.stdout)
