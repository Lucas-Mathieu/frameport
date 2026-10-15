"""One FramePort window at a time (no Flet).

Two GUI processes on one data folder fought over library.json and both re-analysed the library and triaged the same
play sessions (the Library flickered under a stream of pop-ups). The GUI holds an exclusive lock on
`<data>/gui.lock` for its lifetime (released by the OS when the process ends, also after a crash). A second start:
- the window is there (fresh `gui.alive` heartbeat): asks it to come to the front (`gui.focus`) and exits;
- the lock is held but no window answers (a process left over after its window went away): ends that process (only
  when it is a FramePort GUI) and takes over.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

from .paths import user_data_dir, write_atomic

_held = None  # the open lock file (kept for the process lifetime)


def lock_file() -> Path:
    return user_data_dir() / "gui.lock"


def focus_file() -> Path:
    return user_data_dir() / "gui.focus"


def _owner_file(path: Path) -> Path:
    return path.with_suffix(".pid")


def _try_lock(f) -> bool:
    try:
        if sys.platform == "win32":
            import msvcrt

            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def acquire(path: Path | None = None) -> bool:
    """Take the GUI lock; False when another process holds it."""
    global _held
    path = path or lock_file()
    path.parent.mkdir(parents=True, exist_ok=True)
    f = open(path, "a+")  # noqa: SIM115 (kept open on purpose)
    if not _try_lock(f):
        f.close()
        return False
    f.seek(0)
    f.truncate()
    f.write(str(os.getpid()))
    f.flush()
    _held = f
    try:
        import psutil

        # Windows denies reading the locked byte, so another launch cannot read the PID from gui.lock itself.
        write_atomic(_owner_file(path), json.dumps({"pid": os.getpid(), "created": psutil.Process().create_time()}))
    except Exception:  # noqa: BLE001  the lock is ours; a missing owner file only costs a later launch's recovery
        pass
    return True


def release() -> None:
    """Give up the GUI lock and delete its file (uninstall removes the data folder from the running app: Windows
    can't delete a file this process keeps open and locked)."""
    global _held
    f, _held = _held, None
    if f is None:
        return
    path = Path(f.name)
    try:
        _owner_file(path).unlink(missing_ok=True)
    except OSError:
        pass
    try:
        if sys.platform == "win32":
            import msvcrt

            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
    except OSError:
        pass
    f.close()
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def holder(path: Path | None = None) -> int | None:
    path = path or lock_file()
    try:
        import psutil

        owner = json.loads(_owner_file(path).read_text())
        pid = int(owner["pid"])
        if abs(psutil.Process(pid).create_time() - float(owner["created"])) < 0.01:
            return pid  # creation time guards against stale metadata and recycled PIDs
    except (OSError, ValueError, KeyError, TypeError, psutil.Error):
        pass
    try:
        return int(path.read_text().strip() or 0) or None
    except (OSError, ValueError):
        return None


def _is_frameport_gui(pid: int) -> bool:
    try:
        import psutil

        cmd = psutil.Process(pid).cmdline()
    except Exception:  # noqa: BLE001 (gone, or not ours to inspect)
        return False
    if any(a == "-m" and b == "frameport.ui.app" for a, b in zip(cmd, cmd[1:], strict=False)):
        return True
    return any(Path(arg).name.lower() in ("frameport", "frameport.exe", "frameport-gui", "frameport-gui.exe")
               for arg in cmd[:2])


def _legacy_holders(path: Path) -> list[int]:
    """Older Windows GUIs have no readable PID record. Only recover processes holding this exact lock."""
    import psutil

    found = []
    wanted = os.path.normcase(str(path.resolve()))
    for process in psutil.process_iter(["pid"]):
        if process.pid == os.getpid() or not _is_frameport_gui(process.pid):
            continue
        try:
            if any(os.path.normcase(f.path) == wanted for f in process.open_files()):
                found.append(process.pid)
        except (OSError, psutil.Error):
            pass
    return found


STARTUP_GRACE = 30.0  # a GUI this young may still be opening its window (no heartbeat yet): never end it


def end_leftover(pid: int, wait: float = 5.0) -> bool:
    """End a FramePort GUI process that has no window any more; True when it's gone."""
    if pid == os.getpid() or not _is_frameport_gui(pid):
        return False
    import psutil

    try:
        p = psutil.Process(pid)
        if time.time() - p.create_time() < STARTUP_GRACE:
            return False
        p.terminate()
        try:
            p.wait(wait)
        except psutil.TimeoutExpired:
            p.kill()
            p.wait(wait)
    except psutil.NoSuchProcess:
        pass
    except Exception:  # noqa: BLE001
        return False
    return True


def ask_focus() -> None:
    try:
        focus_file().touch()
    except OSError:
        pass


def take_focus_request() -> bool:
    """True once after another start asked the window to come to the front."""
    try:
        focus_file().unlink()
        return True
    except OSError:
        return False


def claim(window_open, retries: int = 20) -> bool:
    """Called at GUI start: True = this process is the GUI; False = another window is open (it was asked to come to
    the front). `window_open()` says whether a window answers (urlhandler.app_running)."""
    if acquire():
        return True
    if window_open():
        ask_focus()
        return False
    pid = holder()
    if pid:
        end_leftover(pid)
    elif sys.platform == "win32":
        for pid in _legacy_holders(lock_file()):
            end_leftover(pid)
    for _ in range(retries):  # the OS releases the lock when the process is gone
        if acquire():
            return True
        time.sleep(0.25)
    if window_open():
        ask_focus()
    return False
