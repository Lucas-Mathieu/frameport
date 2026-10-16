"""Save backups on the PC (GitHub discussion #352): storage, retention, manifests, and the stream to/from the agent,
which runs here for real (a stand-in `podman` whose `unshare` just runs the command)."""
import json
import os
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import pytest

from frameport.core import library
from frameport.install import saves

AGENT = Path(__file__).resolve().parents[1] / "agent" / "frameport_agent.py"
PKG = "com.example.game"


class _Chan:
    def __init__(self, proc):
        self.proc = proc

    def shutdown_write(self):
        if not self.proc.stdin.closed:
            self.proc.stdin.close()

    def recv_exit_status(self):
        return self.proc.wait(30)

    def close(self):
        self.proc.kill()


class _Stream:
    def __init__(self, f, chan):
        self.f, self.channel = f, chan

    def read(self, n=-1):
        return self.f.read(n)

    def write(self, data):
        self.f.write(data)
        self.f.flush()


class FakeFrame:
    """The agent run as a local process over pipes, like an SSH exec channel."""

    def __init__(self, home: Path, env: dict):
        self.home, self.env = home, env
        self.client = self

    def ensure_agent(self):
        return str(AGENT)

    def exec_command(self, cmd, timeout=None):
        proc = subprocess.Popen(["sh", "-c", cmd], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=self.env)
        chan = _Chan(proc)
        return _Stream(proc.stdin, chan), _Stream(proc.stdout, chan), _Stream(proc.stderr, chan)

    def agent(self, command, **args):
        p = subprocess.run([sys.executable, str(AGENT), command], input=json.dumps(args), capture_output=True,
                           text=True, env=self.env)
        reply = json.loads(p.stdout.strip().splitlines()[-1])
        if not reply["ok"]:
            raise RuntimeError(reply["error"])
        return reply["result"]


@pytest.fixture
def frame(tmp_path, monkeypatch):
    home = tmp_path / "frame-home"
    anchor = home / "Applications/quest-frame" / PKG
    (anchor / "lepton-app").mkdir(parents=True)
    (anchor / "deployment.json").write_text(json.dumps({"package": PKG, "appid": 7, "version": "2.0"}))
    internal = anchor / "lepton-data/internal" / PKG
    external = anchor / "lepton-data/external/Android/data" / PKG
    for path, data in ((internal / "shared_prefs/p.xml", "<map/>"), (internal / "files/slot.sav", "level 3"),
                       (external / "files/save.dat", "coins 10"), (external / "cache/x", "cache")):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    podman = bin_dir / "podman"
    podman.write_text('#!/bin/sh\nif [ "$1" = unshare ]; then shift; exec "$@"; fi\n'
                      'if [ "$1" = ps ]; then cat "$FAKE_PS" 2>/dev/null; fi\nexit 0\n')
    podman.chmod(0o755)
    env = dict(os.environ, HOME=str(home), PATH=f"{bin_dir}:{os.environ['PATH']}", FAKE_PS=str(tmp_path / "ps"))
    env.pop("FRAMEPORT_SAVES_NS", None)
    return FakeFrame(home, env), anchor, internal, external, tmp_path


def test_backup_restore_round_trip_through_the_agent(frame):
    fr, anchor, internal, external, tmp = frame
    b = saves.backup(fr, PKG)
    assert b.path.parent == saves.backups_dir(PKG) and b.path.exists() and b.sidecar.exists()
    assert b.files == 3 and b.version == "2.0" and b.bytes == len("<map/>level 3coins 10")
    m = saves.read_manifest(b.path)
    assert {f["path"] for f in m["files"]} == {"internal/shared_prefs/p.xml", "internal/files/slot.sav",
                                               "external/files/save.dat"}
    assert saves.list_backups(PKG)[0].path == b.path
    (external / "files/save.dat").write_text("coins 0")
    r = saves.restore(fr, PKG, b.path)
    assert (external / "files/save.dat").read_text() == "coins 10"
    assert Path(r["rollback"]) == anchor / "saves-before-restore.tar.gz"
    assert saves.info(fr, PKG)["rollback"]["size"] > 0
    saves.undo_restore(fr, PKG)
    assert (external / "files/save.dat").read_text() == "coins 0"


def test_backup_refused_while_the_game_runs(frame):
    fr, anchor, internal, external, tmp = frame
    (tmp / "ps").write_text("lepton-steamlaunch-7\n")
    with pytest.raises(saves.SavesError, match="running"):
        saves.backup(fr, PKG)
    assert not list(saves.backups_dir(PKG).glob("*.part")) and not saves.list_backups(PKG)
    with pytest.raises(saves.SavesError, match="running"):
        saves.restore(fr, PKG, _fake_backup(PKG))


def _fake_backup(pkg: str, created: float | None = None, name: str | None = None) -> Path:
    import io

    d = saves.backups_dir(pkg)
    d.mkdir(parents=True, exist_ok=True)
    path = d / (name or time.strftime("%Y%m%d-%H%M%S.tar.gz"))
    raw = json.dumps({"format": 1, "package": pkg, "created": created or time.time(), "files": []}).encode()
    with tarfile.open(path, "w:gz") as tar:
        ti = tarfile.TarInfo(saves.MANIFEST)
        ti.size = len(raw)
        tar.addfile(ti, io.BytesIO(raw))
    return path


def test_retention_keeps_the_newest(tmp_path):
    for i in range(6):
        _fake_backup(PKG, created=1000 + i, name=f"2026010{i}-120000.tar.gz")
    assert len(saves.list_backups(PKG)) == 6  # sidecars rebuilt from the archives
    assert all(p.with_suffix("").with_suffix(".json").exists() for p in saves.backups_dir(PKG).glob("*.tar.gz"))
    gone = saves.prune(PKG, keep=4)
    assert sorted(p.name for p in gone) == ["20260100-120000.tar.gz", "20260101-120000.tar.gz"]
    left = saves.list_backups(PKG)
    assert [b.created for b in left] == [1005, 1004, 1003, 1002]
    assert not (saves.backups_dir(PKG) / "20260100-120000.json").exists()


def test_keep_setting(tmp_path):
    assert saves.keep_count() == saves.KEEP_DEFAULT
    library.set_setting("saves.keep", 3)
    assert saves.keep_count() == 3
    library.set_setting("saves.keep", "junk")
    assert saves.keep_count() == saves.KEEP_DEFAULT


def test_restore_refuses_a_foreign_or_broken_file_before_touching_the_frame(tmp_path):
    other = _fake_backup("com.other.game")

    class NoFrame:
        def ensure_agent(self):
            raise AssertionError("the Frame must not be contacted")
    with pytest.raises(saves.SavesError, match="com.other.game"):
        saves.restore(NoFrame(), PKG, other)
    junk = tmp_path / "junk.tar.gz"
    junk.write_bytes(b"nope")
    with pytest.raises(ValueError):
        saves.restore(NoFrame(), PKG, junk)


def test_cli_backup_list_restore_undo(frame, monkeypatch):
    from types import SimpleNamespace

    from typer.testing import CliRunner

    from frameport import cli

    fr, anchor, internal, external, tmp = frame
    target = SimpleNamespace(frame=fr, installed=lambda: [{"package": PKG}, {"package": "rift.x", "kind": "pcvr"}])
    monkeypatch.setattr(cli, "_target", lambda frame=None, password=None, to="frame": target)
    run = CliRunner().invoke
    r = run(cli.app, ["saves", "backup", "--all"])
    assert r.exit_code == 0, r.output
    assert f"{PKG}: 3 file(s)" in r.output and "rift.x" not in r.output
    r = run(cli.app, ["saves", "list", PKG])
    assert r.exit_code == 0 and r.output.lstrip().startswith("1  ")
    (external / "files/save.dat").write_text("lost")
    r = run(cli.app, ["saves", "restore", PKG, "1", "--yes"])
    assert r.exit_code == 0, r.output
    assert (external / "files/save.dat").read_text() == "coins 10"
    r = run(cli.app, ["saves", "undo", PKG])
    assert r.exit_code == 0, r.output
    assert (external / "files/save.dat").read_text() == "lost"
    r = run(cli.app, ["saves", "restore", PKG, "7", "--yes"])
    assert r.exit_code == 2  # no 7th backup
    (tmp / "ps").write_text("lepton-steamlaunch-7\n")
    r = run(cli.app, ["saves", "backup", PKG])
    assert r.exit_code == 1 and "running" in r.output


def test_bad_package_names_never_make_paths():
    with pytest.raises(ValueError):
        saves.backups_dir("../../etc")
    assert saves.has_backups("../x") is False
