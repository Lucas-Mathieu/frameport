"""One FramePort window at a time (core/instance.py)."""
import json
import os
import subprocess
import sys
import textwrap
from pathlib import Path
from types import SimpleNamespace

import pytest

from frameport.core import instance


def _hold_lock(path):
    """A separate process holding the GUI lock (like a running FramePort)."""
    code = textwrap.dedent(f"""
        import os, sys, time
        sys.path.insert(0, {str(instance.__file__.rsplit('/frameport/', 1)[0])!r})
        from pathlib import Path
        from frameport.core import instance
        assert instance.acquire(Path({str(path)!r}))
        print("locked", os.getpid(), flush=True)
        time.sleep(30)
    """)
    p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    status = p.stdout.readline().strip().split()
    assert status[0] == "locked"
    p.gui_pid = int(status[1])  # Windows venv's python.exe can launch another interpreter process
    return p


def _end_holder(holder):
    import psutil

    try:
        psutil.Process(holder.gui_pid).kill()
    except psutil.NoSuchProcess:
        pass
    holder.wait(5)


def test_second_start_is_refused_while_a_window_is_open(tmp_path, monkeypatch):
    monkeypatch.setenv("FRAMEPORT_HOME", str(tmp_path))
    holder = _hold_lock(instance.lock_file())
    try:
        assert instance.holder() == holder.gui_pid
        assert not instance.acquire()
        assert instance.claim(lambda: True) is False  # a window answers: it's asked to come to the front
        assert instance.take_focus_request() and not instance.take_focus_request()
        # no window answers, but the holder isn't a FramePort GUI (or is still starting): it's left alone
        assert instance.claim(lambda: False, retries=1) is False
        assert holder.poll() is None
    finally:
        _end_holder(holder)
    assert instance.acquire()  # released by the OS when the holder ends


def test_leftover_frameport_gui_is_ended(tmp_path, monkeypatch):
    monkeypatch.setenv("FRAMEPORT_HOME", str(tmp_path))
    holder = _hold_lock(instance.lock_file())
    monkeypatch.setattr(instance, "_is_frameport_gui", lambda pid: pid == holder.gui_pid)
    monkeypatch.setattr(instance, "STARTUP_GRACE", 0.0)
    try:
        assert instance.claim(lambda: False) is True  # no window: the leftover process is ended, the lock taken
        assert holder.wait(5) is not None
    finally:
        if holder.poll() is None:
            _end_holder(holder)


def test_holder_is_readable_when_windows_blocks_the_lock_contents(tmp_path, monkeypatch):
    path = tmp_path / "gui.lock"
    assert instance.acquire(path)
    read_text = Path.read_text

    def locked(self, *args, **kwargs):
        if self == path:
            raise PermissionError("Windows byte-range lock")
        return read_text(self, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", locked)
    try:
        assert instance.holder(path) == os.getpid()
        owner = instance._owner_file(path)
        data = json.loads(owner.read_text())
        data["created"] -= 100  # a PID reused by another process must not be accepted
        owner.write_text(json.dumps(data))
        assert instance.holder(path) is None
    finally:
        instance.release()
    assert not instance._owner_file(path).exists()


@pytest.mark.parametrize("cmd,expected", [
    (["python", "-m", "frameport.ui.app"], True),
    (["python", "-m", "frameport.cli"], False),
    (["python", "-c", "import frameport"], False),
    (["/apps/FramePort.exe"], True),
])
def test_recovery_only_matches_gui_commands(monkeypatch, cmd, expected):
    import psutil

    monkeypatch.setattr(psutil, "Process", lambda pid: SimpleNamespace(cmdline=lambda: cmd))
    assert instance._is_frameport_gui(123) is expected


def test_legacy_recovery_only_finds_the_same_data_folders_lock(tmp_path, monkeypatch):
    import psutil

    lock = tmp_path / "gui.lock"
    other = tmp_path / "another-app/gui.lock"
    processes = [SimpleNamespace(pid=123, open_files=lambda: [SimpleNamespace(path=str(lock))]),
                 SimpleNamespace(pid=124, open_files=lambda: [SimpleNamespace(path=str(other))])]
    monkeypatch.setattr(psutil, "process_iter", lambda attrs: processes)
    monkeypatch.setattr(instance, "_is_frameport_gui", lambda pid: True)
    assert instance._legacy_holders(lock) == [123]


def test_claim_recovers_a_legacy_windows_holder_without_readable_pid(monkeypatch):
    monkeypatch.setattr(instance, "sys", SimpleNamespace(platform="win32"))
    attempts = iter([False, True])
    ended = []
    monkeypatch.setattr(instance, "acquire", lambda: next(attempts))
    monkeypatch.setattr(instance, "holder", lambda: None)
    monkeypatch.setattr(instance, "_legacy_holders", lambda path: [123])
    monkeypatch.setattr(instance, "end_leftover", lambda pid: ended.append(pid))
    assert instance.claim(lambda: False)
    assert ended == [123]


def test_lock_is_kept_when_the_owner_file_cannot_be_written(tmp_path, monkeypatch):
    """An antivirus or a read-only file must not stop the GUI from starting once it holds the lock."""
    def fail(*args, **kwargs):
        raise PermissionError("locked by another program")

    monkeypatch.setattr(instance, "write_atomic", fail)
    try:
        assert instance.acquire(tmp_path / "gui.lock")
    finally:
        instance.release()
