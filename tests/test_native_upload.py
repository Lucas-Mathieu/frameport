"""Bulk backend selection, paired-host trust, resumable native batches and cancellation."""
import contextlib
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from frameport.core.events import Cancelled, Reporter
from frameport.frame import native_upload as native
from frameport.frame.connection import Frame, FrameTarget
from frameport.install import installer


def test_pinned_credentials(tmp_path, monkeypatch):
    monkeypatch.setattr(native, "ssh_dir", lambda: tmp_path)
    (tmp_path / "id_ed25519").write_text("private")
    key = SimpleNamespace(get_name=lambda: "ssh-ed25519", get_base64=lambda: "PAIRED_KEY")
    client = SimpleNamespace(get_transport=lambda: SimpleNamespace(get_remote_server_key=lambda: key))
    frame = Frame(FrameTarget("paired.example", port=2222), client=client)
    with native.credentials(frame) as args:
        known = next(arg.split("=", 1)[1].strip('"') for arg in args if arg.startswith("UserKnownHostsFile="))
        from pathlib import Path
        assert Path(known).read_text() == "[paired.example]:2222 ssh-ed25519 PAIRED_KEY\n"
        assert "StrictHostKeyChecking=yes" in args
        assert "BatchMode=yes" in args
        copy = args[args.index("-i") + 1]
        assert Path(copy).read_text() == "private"
        assert Path(copy) != tmp_path / "id_ed25519"
    assert not Path(copy).exists()
    assert (tmp_path / "id_ed25519").read_text() == "private"


@pytest.mark.parametrize("value", ["bad\npath", "bad\rpath", "bad\x00path"])
def test_quote_refuses_batch_injection(value):
    with pytest.raises(ValueError):
        native.quote(value)


def test_quote_protects_spaces_quotes_globs():
    assert native.quote('a *?["b') == '"a \\*\\?\\[\\"b"'


def test_size_lookup_does_not_hide_connection_errors():
    sftp = Mock()
    sftp.stat.side_effect = ConnectionError("lost connection")
    with pytest.raises(ConnectionError):
        native.size_of(sftp, "/file")


def setup_batch(tmp_path, monkeypatch, code=0):
    paths = [tmp_path / "main.obb", tmp_path / "patch.obb"]
    for path in paths:
        path.write_bytes(b"0123456789")
    items = [(path, path.name, 10) for path in paths]
    files = {"/incoming/main.obb.part": 4, "/incoming/patch.obb.part": 20}
    monitor = Mock()
    monitor.__enter__ = Mock(return_value=monitor)
    monitor.__exit__ = Mock(return_value=False)
    def stat(path):
        if path not in files:
            raise FileNotFoundError(path)
        return SimpleNamespace(st_size=files[path])
    monitor.stat.side_effect = stat
    monitor.remove.side_effect = lambda path: files.pop(path)
    frame = SimpleNamespace(sftp=monitor, client=SimpleNamespace(open_sftp=lambda: monitor),
                            target=FrameTarget("paired"), run=Mock(return_value=(0, "", "")))
    monkeypatch.setattr(native.shutil, "which", lambda _: "sftp")
    @contextlib.contextmanager
    def creds(frame):
        yield ["-o", "StrictHostKeyChecking=yes"]
    monkeypatch.setattr(native, "credentials", creds)
    commands, processes = [], []
    def popen(args, **kwargs):
        commands.append(kwargs["stdin"].read().decode())
        for _, rel, size in items:
            files["/incoming/" + rel + ".part"] = size
        process = Mock(returncode=code)
        process.poll.return_value = code
        processes.append(process)
        return process
    monkeypatch.setattr(native.subprocess, "Popen", popen)
    return frame, items, files, commands, processes


def test_native_batches_resume_reset_oversize_and_verify(tmp_path, monkeypatch):
    frame, items, files, commands, processes = setup_batch(tmp_path, monkeypatch)
    assert native.upload(frame, items, "/incoming", Reporter(), 20, 0, 3) == 20
    assert len(processes) == 2  # never more processes than files
    assert commands[0].startswith("reput ")
    assert commands[1].startswith("put ")
    assert all(".part" in command for command in commands)
    frame.sftp.remove.assert_called_once_with("/incoming/patch.obb.part")
    assert frame.run.call_count == 1
    assert all("mv -f --" in call.args[0] for call in frame.run.call_args_list)
    frame.sftp.get_channel().settimeout.assert_called_once_with(2)
    assert all(process.wait.called for process in processes)


def test_native_failure_does_not_promote_files(tmp_path, monkeypatch):
    frame, items, files, commands, processes = setup_batch(tmp_path, monkeypatch, code=1)
    with pytest.raises(OSError, match="OpenSSH SFTP failed"):
        native.upload(frame, items, "/incoming", Reporter(), 20, 0, 2)
    assert not frame.run.called
    assert files["/incoming/main.obb.part"] == 10
    assert all(process.wait.called for process in processes)


def test_native_success_still_checks_remote_sizes(tmp_path, monkeypatch):
    frame, items, files, commands, processes = setup_batch(tmp_path, monkeypatch)
    original = native.subprocess.Popen
    def truncated(*args, **kwargs):
        process = original(*args, **kwargs)
        files["/incoming/main.obb.part"] = 9
        return process
    monkeypatch.setattr(native.subprocess, "Popen", truncated)
    with pytest.raises(OSError, match="unexpected size"):
        native.upload(frame, items, "/incoming", Reporter(), 20, 0, 2)
    assert not frame.run.called
    assert all(process.wait.called for process in processes)


def test_native_cancel_stops_owned_processes(tmp_path, monkeypatch):
    frame, items, files, commands, processes = setup_batch(tmp_path, monkeypatch)
    report = Reporter()
    report.progress = lambda *args, **kwargs: report.cancelled.set()
    monkeypatch.setattr(native, "stop_process", Mock())
    with pytest.raises(Cancelled):
        native.upload(frame, items, "/incoming", report, 20, 0, 2)
    assert not frame.run.called
    assert native.stop_process.call_count == 2


@pytest.mark.parametrize("cancel", [False, True])
def test_production_fallback_and_cancel(tmp_path, monkeypatch, cancel):
    path = tmp_path / "game.apk"
    path.write_bytes(b"apk")
    frame = Frame(FrameTarget("paired"))
    frame.mkdirs = Mock()
    frame.put = Mock()
    report = Reporter()
    fractions = []
    report.progress = lambda fraction, *args, **kwargs: fractions.append(fraction)
    def fail(frame, items, remote, reporter, *args):
        reporter.progress(0.8, "native")
        if cancel:
            raise Cancelled()
        raise OSError("native unavailable")
    def fallback(local, remote, progress, **kwargs):
        progress(0, installer.BIG_FILE)
        progress(installer.BIG_FILE, installer.BIG_FILE)
    frame.put.side_effect = fallback
    monkeypatch.setattr(native, "upload", fail)
    call = lambda: installer.upload_files(frame, [(path, "game.apk", installer.BIG_FILE)],
                                          "/incoming", report, installer.BIG_FILE, workers=1)
    if cancel:
        with pytest.raises(Cancelled):
            call()
        assert not frame.put.called
    else:
        assert call() == installer.BIG_FILE
        frame.put.assert_called_once()
        assert fractions == sorted(fractions)
