"""Bulk backend selection, paired-host trust, resumable native batches and cancellation."""
import contextlib
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from frameport.core.events import Cancelled, Reporter
from frameport.frame import native_upload as native
from frameport.frame.connection import Frame, FrameTarget
from frameport.install import installer


@pytest.mark.parametrize("port_flag", ["-P", "-p"])
def test_pinned_credentials(tmp_path, monkeypatch, port_flag):
    monkeypatch.setattr(native, "ssh_dir", lambda: tmp_path)
    (tmp_path / "id_ed25519").write_text("private")
    key = SimpleNamespace(get_name=lambda: "ssh-ed25519", get_base64=lambda: "PAIRED_KEY")
    client = SimpleNamespace(get_transport=lambda: SimpleNamespace(get_remote_server_key=lambda: key))
    frame = Frame(FrameTarget("paired.example", port=2222), client=client)
    with native.credentials(frame, port_flag=port_flag) as args:
        known = next(arg.split("=", 1)[1].strip('"') for arg in args if arg.startswith("UserKnownHostsFile="))
        from pathlib import Path
        assert Path(known).read_text() == "[paired.example]:2222 ssh-ed25519 PAIRED_KEY\n"
        assert "StrictHostKeyChecking=yes" in args
        assert "BatchMode=yes" in args
        assert args[-2:] == [port_flag, "2222"]
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


def test_native_complete_parts_are_only_renamed(tmp_path, monkeypatch):
    """A .part as big as its file (left by a cancelled run) isn't sent again: sftp's reput refuses it."""
    frame, items, files, commands, processes = setup_batch(tmp_path, monkeypatch)
    files["/incoming/main.obb.part"] = 10
    assert native.upload(frame, items, "/incoming", Reporter(), 20, 0, 3) == 20
    assert len(processes) == 1 and "main.obb" not in commands[0] and commands[0].startswith("put ")
    assert "main.obb.part" in frame.run.call_args.args[0]
    files["/incoming/patch.obb.part"] = 10
    commands.clear()
    processes.clear()
    assert native.upload(frame, items, "/incoming", Reporter(), 20, 0, 3) == 20
    assert not processes and frame.run.call_count == 2


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


@pytest.mark.parametrize("failure", [None, OSError("ssh failed"), Cancelled()])
def test_tar_transport_selection(tmp_path, monkeypatch, failure):
    path = tmp_path / "small.bin"
    path.write_bytes(b"payload")
    frame = Frame(FrameTarget("paired.example"))
    frame.mkdirs = Mock()
    frame.put_tar = Mock()
    events = []
    report = Reporter([events.append])
    stream = Mock(side_effect=failure)
    monkeypatch.setattr(native, "put_tar", stream)
    args = (frame, [(path, "data/small.bin", 7)], "/incoming", report, 7)
    if isinstance(failure, Cancelled):
        with pytest.raises(Cancelled):
            installer.upload_files(*args)
        frame.put_tar.assert_not_called()
    else:
        assert installer.upload_files(*args) == 7
        assert frame.put_tar.called == (failure is not None)
        logs = [event.message for event in events if event.kind == "log"]
        assert any("tar batch with OpenSSH" in message for message in logs)
        if failure:
            assert any("OpenSSH unavailable or failed; resuming with Paramiko" in message for message in logs)
            assert any("tar batch with Paramiko" in message for message in logs)
    stream.assert_called_once()


def setup_tar(tmp_path, monkeypatch, code=0):
    import io

    path = tmp_path / "small.bin"
    path.write_bytes(b"payload")
    frame = SimpleNamespace(target=FrameTarget("paired.example", port=2222))
    pipe = io.BytesIO()
    pipe.close = lambda: None
    process = Mock(stdin=pipe, returncode=code)
    process.poll.return_value = code
    popen = Mock(return_value=process)
    monkeypatch.setattr(native.subprocess, "Popen", popen)
    monkeypatch.setattr(native.shutil, "which", lambda _: "ssh")
    @contextlib.contextmanager
    def creds(frame, port_flag):
        assert port_flag == "-p"
        yield ["-o", "StrictHostKeyChecking=yes", port_flag, "2222"]
    monkeypatch.setattr(native, "credentials", creds)
    return frame, [(path, "nested/small.bin")], process, popen, pipe


def test_native_tar_stream_and_command(tmp_path, monkeypatch):
    import tarfile

    frame, files, process, popen, pipe = setup_tar(tmp_path, monkeypatch)
    progress = Mock()
    native.put_tar(frame, files, "/incoming with 'quote", Reporter(), progress)
    args = popen.call_args.args[0]
    assert args == ["ssh", "-o", "StrictHostKeyChecking=yes", "-p", "2222", "steamos@paired.example",
                    "mkdir -p '/incoming with '\"'\"'quote' && tar -xf - -C '/incoming with '\"'\"'quote'"]
    pipe.seek(0)
    with tarfile.open(fileobj=pipe) as archive:
        assert archive.getnames() == ["nested/small.bin"]
        assert archive.extractfile("nested/small.bin").read() == b"payload"
    progress.assert_called_once_with(7, "nested/small.bin")
    process.wait.assert_called_once()


def test_native_tar_failed_exit(tmp_path, monkeypatch):
    frame, files, process, popen, pipe = setup_tar(tmp_path, monkeypatch, code=1)
    with pytest.raises(OSError, match="OpenSSH tar failed"):
        native.put_tar(frame, files, "/incoming", Reporter(), Mock())
    process.wait.assert_called_once()


def test_native_tar_cancel_blocked_writer(tmp_path, monkeypatch):
    import threading

    frame, files, process, popen, pipe = setup_tar(tmp_path, monkeypatch)
    report = Reporter()
    released = threading.Event()
    def blocked_write(data):
        report.cancelled.set()
        assert released.wait(5)
        raise BrokenPipeError()
    pipe.write = blocked_write
    pipe.close = Mock(side_effect=BrokenPipeError())
    process.poll.return_value = None
    stop = Mock(side_effect=lambda process: released.set())
    monkeypatch.setattr(native, "stop_process", stop)
    with pytest.raises(Cancelled):
        native.put_tar(frame, files, "/incoming", report, Mock())
    stop.assert_called_once_with(process)


@pytest.mark.parametrize("prefer_native", [False, True])
def test_tar_unavailable_and_explicit_paramiko(tmp_path, monkeypatch, prefer_native):
    path = tmp_path / "small.bin"
    path.write_bytes(b"payload")
    frame = Frame(FrameTarget("paired.example"))
    frame.mkdirs = Mock()
    frame.put_tar = Mock()
    monkeypatch.setattr(native.shutil, "which", lambda _: None)
    monkeypatch.setattr(installer, "BATCH_FILES", 1)
    stream = Mock(wraps=native.put_tar)
    monkeypatch.setattr(native, "put_tar", stream)
    events = []
    assert installer.upload_files(frame, [(path, "one", 7), (path, "two", 7)], "/incoming",
                                  Reporter([events.append]), 14, prefer_native=prefer_native) == 14
    assert stream.call_count == int(prefer_native)  # a failed backend isn't retried for every batch
    assert frame.put_tar.call_count == 2
    assert sum("OpenSSH unavailable or failed" in event.message for event in events) == int(prefer_native)
