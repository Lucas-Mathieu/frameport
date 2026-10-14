"""Bounded independent upload sessions, aggregate progress and cancellation of blocked workers."""
import threading
from types import SimpleNamespace

import pytest

from frameport.core import library
from frameport.core.events import Cancelled, Reporter
from frameport.install import installer


@pytest.mark.parametrize("value, expected", [(1, 1), (2, 2), (8, 8), (0, 1), (99, 8),
                                             ("3", 3), ("bad", 3), (float("inf"), 3)])
def test_upload_threads_bounds(value, expected):
    assert installer.upload_threads(value) == expected


def test_upload_threads_default_and_setting():
    assert installer.upload_threads() == 3
    library.set_setting("install.upload_threads", 8)
    assert installer.upload_threads() == 8


def items(tmp_path, count=4):
    result = []
    for n in range(count):
        path = tmp_path / f"main-{n}.obb"
        path.write_bytes(b"a" * 10)
        result.append((path, f"obb/{path.name}", 10))
    return result


class Control:
    def __init__(self, action):
        self.action = action
        self.sessions, self.calls, self.tars, self.dirs = [], [], [], []
        self.lock = threading.Lock()

    def mkdirs(self, dirs):
        self.dirs.extend(dirs)

    def upload_session(self):
        owner = self
        class Session:
            def __init__(self):
                self.closed = threading.Event()

            def put(self, path, remote, progress, **kwargs):
                progress(0, 10)
                owner.action(self, path, progress)
                with owner.lock:
                    owner.calls.append((remote, kwargs))

            def close(self):
                self.closed.set()
        session = Session()
        with self.lock:
            self.sessions.append(session)
        return session

    def put(self, path, remote, progress, **kwargs):
        self.calls.append((remote, kwargs))
        progress(0, 10)
        progress(10, 10)

    def put_tar(self, files, root, progress):
        self.tars.append([rel for _, rel in files])
        progress(sum(path.stat().st_size for path, _ in files), files[-1][1])

    def close(self):
        pytest.fail("the control connection must remain open")


def test_two_workers_and_monotonic_aggregate_progress(monkeypatch, tmp_path):
    monkeypatch.setattr(installer, "BIG_FILE", 5)
    barrier = threading.Barrier(2)
    activity = {"active": 0, "peak": 0}
    lock = threading.Lock()
    def action(session, path, progress):
        with lock:
            activity["active"] += 1
            activity["peak"] = max(activity["peak"], activity["active"])
        barrier.wait(timeout=3)
        progress(5, 10)
        progress(10, 10)
        with lock:
            activity["active"] -= 1
    frame = Control(action)
    events = []
    reporter = Reporter([events.append])
    assert installer.upload_files(frame, items(tmp_path), "/incoming", reporter, 40, workers=2) == 40
    assert activity["peak"] == 2
    assert len(frame.sessions) == 2 and all(session.closed.is_set() for session in frame.sessions)
    assert sorted(remote for remote, _ in frame.calls) == [f"/incoming/obb/main-{n}.obb" for n in range(4)]
    assert all(kwargs["mkdir"] is False and callable(kwargs["check_cancel"]) for _, kwargs in frame.calls)
    fractions = [event.fraction for event in events if event.kind == "progress"]
    assert fractions == sorted(fractions) and fractions[-1] == 1.0


def test_one_worker_uses_existing_serial_transport(monkeypatch, tmp_path):
    monkeypatch.setattr(installer, "BIG_FILE", 5)
    frame = Control(lambda *args: pytest.fail("no worker transport needed"))
    installer.upload_files(frame, items(tmp_path), "/incoming", Reporter(), 40, workers=1)
    assert not frame.sessions and len(frame.calls) == 4


def test_worker_count_is_limited_to_large_files_and_tar_is_sequential(monkeypatch, tmp_path):
    monkeypatch.setattr(installer, "BIG_FILE", 5)
    barrier = threading.Barrier(2)
    def action(session, path, progress):
        assert not frame.tars
        barrier.wait(timeout=3)
        progress(10, 10)
    frame = Control(action)
    selected = items(tmp_path, 2)
    small = tmp_path / "small.txt"
    small.write_bytes(b"x")
    selected.append((small, "obb/small.txt", 1))
    assert installer.upload_files(frame, selected, "/incoming", Reporter(), 21, workers=8) == 21
    assert len(frame.sessions) == 2
    assert frame.tars == [["obb/small.txt"]]


def test_cancel_interrupts_blocked_workers_and_stops_queued_files(monkeypatch, tmp_path):
    monkeypatch.setattr(installer, "BIG_FILE", 5)
    started = threading.Barrier(3)
    def action(session, path, progress):
        started.wait(timeout=3)
        assert session.closed.wait(3), "cancellation must close blocked worker sockets"
        raise OSError("transport closed")
    frame, reporter, errors = Control(action), Reporter(), []
    def run():
        try:
            installer.upload_files(frame, items(tmp_path), "/incoming", reporter, 40, workers=2)
        except Exception as exc:
            errors.append(exc)
    thread = threading.Thread(target=run)
    thread.start()
    started.wait(timeout=3)
    reporter.cancelled.set()
    thread.join(timeout=3)
    assert not thread.is_alive()
    assert len(errors) == 1 and isinstance(errors[0], Cancelled)
    assert all(session.closed.is_set() for session in frame.sessions)
    assert not frame.calls


def test_failure_propagates_and_closes_sessions(monkeypatch, tmp_path):
    monkeypatch.setattr(installer, "BIG_FILE", 5)
    def fail(session, path, progress):
        raise OSError("disk full")
    frame = Control(fail)
    with pytest.raises(OSError, match="disk full"):
        installer.upload_files(frame, items(tmp_path), "/incoming", Reporter(), 40, workers=2)
    assert all(session.closed.is_set() for session in frame.sessions)
    assert not frame.calls


def test_parallel_upload_setting_options_and_persistence():
    import flet as ft

    from frameport.ui.views.settings import SettingsView

    app = SimpleNamespace(target=None, frame_state="disconnected")
    controls = SettingsView(app).installing().controls
    dropdown = next(control for control in controls if isinstance(control, ft.Dropdown))
    assert dropdown.value == "3"
    assert [option.key for option in dropdown.options] == [str(n) for n in range(1, 9)]
    dropdown.on_select(SimpleNamespace(control=SimpleNamespace(value="8")))
    assert library.setting("install.upload_threads") == 8


def test_upload_session_checks_paired_headset_key_and_keeps_control(monkeypatch):
    import paramiko

    from frameport.frame.connection import Frame, FrameTarget

    closed, children = [], []
    def client(key):
        return SimpleNamespace(get_transport=lambda: SimpleNamespace(get_remote_server_key=lambda: key),
                               close=lambda: closed.append(key))
    control = Frame(FrameTarget("frame"))
    control.client = client("paired")
    def connect(other):
        other.client = client("other headset")
        children.append(other)
        return other
    monkeypatch.setattr(Frame, "connect", connect)
    with pytest.raises(paramiko.SSHException, match="different headset"):
        control.upload_session()
    assert closed == ["other headset"]
    assert control.client.get_transport().get_remote_server_key() == "paired"
    assert children[0].target is not control.target


def test_closed_worker_does_not_retry_after_cancellation(monkeypatch, tmp_path):
    from frameport.frame.connection import Frame, FrameTarget

    frame = Frame(FrameTarget("frame"))
    calls = []
    cancelled = threading.Event()
    def put(*args):
        calls.append("put")
        cancelled.set()
        raise OSError("socket closed")
    def check():
        if cancelled.is_set():
            raise Cancelled()
    monkeypatch.setattr(frame, "_put", put)
    monkeypatch.setattr(frame, "connect", lambda *args: pytest.fail("must not reconnect a cancelled worker"))
    with pytest.raises(Cancelled):
        frame.put(tmp_path / "file", "/incoming/file", check_cancel=check)
    assert calls == ["put"]
