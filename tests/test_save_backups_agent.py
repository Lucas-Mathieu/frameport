"""Save backups on the Frame (GitHub discussion #352): the agent's archive builder, the untrusted-archive checks on
restore, refusing while the game runs, and the rollback copy. podman unshare is switched off (files are ours here)."""
import importlib.util
import io
import json
import os
import tarfile
from pathlib import Path

import pytest

AGENT = Path(__file__).resolve().parents[1] / "agent" / "frameport_agent.py"
PKG = "com.example.game"


def load_agent(monkeypatch, tmp_path):
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.delenv("FRAMEPORT_SAVES_NS", raising=False)
    spec = importlib.util.spec_from_file_location("frameport_agent_saves", AGENT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    monkeypatch.setattr(mod, "as_owner", lambda: [])
    monkeypatch.setattr(mod, "container_running", lambda appid: False)
    return mod


def write(path: Path, data, mode=0o660):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    os.chmod(path, mode)
    return path


@pytest.fixture
def game(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    anchor = Path(a.ANCHORS) / PKG
    anchor.mkdir(parents=True)
    (anchor / "lepton-app").mkdir()
    (anchor / "deployment.json").write_text(json.dumps({"package": PKG, "appid": 123, "version": "1.2.3",
                                                         "title": "Example"}))
    internal = anchor / "lepton-data/internal" / PKG
    external = anchor / "lepton-data/external/Android/data" / PKG
    write(internal / "shared_prefs/prefs.xml", "<map><int name='level' value='7'/></map>")
    write(internal / "files/UnrealGame/G/G/Saved/SaveGames/slot1.sav", b"\x01" * 100)
    write(internal / "files/UnrealGame/G/G/Saved/Logs/G.log", "log")
    write(internal / "cache/tmp.bin", b"x" * 10)
    write(internal / "code_cache/x", b"x")
    write(internal / "databases/game.db", b"db")
    write(external / "files/SaveData.es3", "save")
    write(external / "files/fb_snap_0.ppm", b"P6")
    write(external / "files/framebridge.conf", "scale=1")
    write(external / "files/il2cpp/Metadata/global-metadata.dat", b"m" * 50)
    write(external / "files/VulkanProgramBinaryCache/x.bin", b"v")
    write(external / "files/Video/intro.mp4", b"vid")
    write(external / "files/big.bundle", b"b" * 2000)
    write(external / "cache/ga.sqlite3", b"g")
    os.symlink("/etc", external / "files/escape")
    return a, anchor, internal, external


def backup_bytes(a, max_file=1000):
    out, err = io.BytesIO(), io.StringIO()
    assert a.saves_backup_session({"package": PKG, "max_file": max_file}, out, err) == 0, err.getvalue()
    reply = json.loads(err.getvalue().strip().splitlines()[-1])
    assert reply["ok"]
    return out.getvalue(), reply["result"]


def names(data: bytes) -> list[str]:
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        return tar.getnames()


def manifest_of(data: bytes) -> dict:
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        return json.loads(tar.extractfile("frameport-saves.json").read())


def test_backup_holds_saves_and_leaves_caches_out(game):
    a, anchor, internal, external = game
    data, summary = backup_bytes(a)
    got = names(data)
    assert got[-1] == "frameport-saves.json"
    files = {n for n in got if not n.endswith(".json")}
    assert "internal/shared_prefs/prefs.xml" in files
    assert "internal/files/UnrealGame/G/G/Saved/SaveGames/slot1.sav" in files
    assert "internal/databases/game.db" in files
    assert "external/files/SaveData.es3" in files
    for left_out in ("internal/cache/tmp.bin", "internal/code_cache/x",
                     "internal/files/UnrealGame/G/G/Saved/Logs/G.log",
                     "external/files/fb_snap_0.ppm", "external/files/framebridge.conf",
                     "external/files/il2cpp/Metadata/global-metadata.dat",
                     "external/files/VulkanProgramBinaryCache/x.bin", "external/files/Video/intro.mp4",
                     "external/files/big.bundle", "external/cache/ga.sqlite3", "external/files/escape"):
        assert left_out not in files, left_out
    m = manifest_of(data)
    assert m["package"] == PKG and m["version"] == "1.2.3" and m["format"] == 1
    by_path = {f["path"]: f for f in m["files"]}
    assert by_path["internal/files/UnrealGame/G/G/Saved/SaveGames/slot1.sav"]["size"] == 100
    assert len(by_path["external/files/SaveData.es3"]["sha256"]) == 64
    assert by_path["internal/shared_prefs/prefs.xml"]["mode"] == 0o660
    reasons = {s["path"]: s["reason"] for s in m["skipped"]}
    assert reasons["external/files/big.bundle"] == "size" and reasons["external/files/escape"] == "link"
    assert summary["files"] == len(m["files"])
    assert [s["path"] for s in summary["skipped"]] == ["external/files/big.bundle"]


def test_backup_refused_while_the_game_runs(game, monkeypatch):
    a, *_ = game
    monkeypatch.setattr(a, "container_running", lambda appid: True)
    out, err = io.BytesIO(), io.StringIO()
    assert a.saves_backup_session({"package": PKG}, out, err) == 1
    assert out.getvalue() == b"" and "running" in json.loads(err.getvalue())["error"]


def test_backup_and_restore_refused_while_the_launcher_holds_its_lock(game):
    import fcntl

    a, anchor, *_ = game
    fd = os.open(anchor / ".launch.lock", os.O_RDWR | os.O_CREAT)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        err = io.StringIO()
        assert a.saves_backup_session({"package": PKG}, io.BytesIO(), err) == 1
        assert "running" in err.getvalue()
        out = io.StringIO()
        assert a.saves_restore_session({"package": PKG}, io.BytesIO(b""), out) == 1
        assert "running" in out.getvalue()
    finally:
        os.close(fd)


def test_backup_refuses_pc_vr_and_unknown_games(game):
    a, anchor, *_ = game
    err = io.StringIO()
    assert a.saves_backup_session({"package": "com.not.installed"}, io.BytesIO(), err) == 1
    assert "not installed" in err.getvalue()
    err = io.StringIO()
    assert a.saves_backup_session({"package": "../etc"}, io.BytesIO(), err) == 1


def restore(a, data: bytes, **extra) -> dict:
    out = io.StringIO()
    a.saves_restore_session({"package": PKG, **extra}, io.BytesIO(data), out)
    return json.loads(out.getvalue().strip().splitlines()[-1])


def test_restore_round_trip_keeps_modes_and_makes_a_rollback_copy(game):
    a, anchor, internal, external = game
    data, _ = backup_bytes(a)
    save = external / "files/SaveData.es3"
    prefs = internal / "shared_prefs/prefs.xml"
    os.chmod(prefs, 0o600)  # the Frame's current file keeps its own mode
    save.write_text("newer progress")
    write(internal / "files/new_slot.sav", "made after the backup")
    big = external / "files/big.bundle"
    reply = restore(a, data)
    assert reply["ok"], reply
    r = reply["result"]
    assert save.read_text() == "save"
    assert not (internal / "files/new_slot.sav").exists()  # replaced, not merged
    assert big.exists() and (internal / "cache/tmp.bin").exists()  # left-out files stay
    assert (prefs.stat().st_mode & 0o777) == 0o600
    assert r["restored"] >= 4 and r["removed"] == 1
    rollback = anchor / "saves-before-restore.tar.gz"
    assert Path(r["rollback"]) == rollback and rollback.exists()
    assert "internal/files/new_slot.sav" in names(rollback.read_bytes())
    assert not list(anchor.glob(".saves-restore-*"))  # staging cleaned up
    # undo: the rollback copy goes back, and the restored state becomes the new rollback copy
    reply = restore(a, b"", rollback=True)
    assert reply["ok"], reply
    assert save.read_text() == "newer progress" and (internal / "files/new_slot.sav").exists()
    assert "internal/files/new_slot.sav" not in names(rollback.read_bytes())


def test_restore_onto_a_fresh_install_creates_the_folders(game):
    import shutil

    a, anchor, internal, external = game
    data, _ = backup_bytes(a)
    shutil.rmtree(anchor / "lepton-data")
    reply = restore(a, data)
    assert reply["ok"], reply
    assert (internal / "files/UnrealGame/G/G/Saved/SaveGames/slot1.sav").read_bytes() == b"\x01" * 100
    assert (external / "files/SaveData.es3").read_text() == "save"
    assert internal.is_dir() and not (external / "cache").exists()


def evil_archive(members: list[tuple[tarfile.TarInfo, bytes | None]], manifest: dict | None = None) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for ti, data in members:
            tar.addfile(ti, io.BytesIO(data) if data is not None else None)
        if manifest is not None:
            raw = json.dumps(manifest).encode()
            ti = tarfile.TarInfo("frameport-saves.json")
            ti.size = len(raw)
            tar.addfile(ti, io.BytesIO(raw))
    return buf.getvalue()


def reg(name: str, data: bytes) -> tuple[tarfile.TarInfo, bytes]:
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    return ti, data


def good_manifest(*files: tuple[str, bytes]) -> dict:
    import hashlib

    return {"format": 1, "package": PKG, "files": [{"path": n, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()}
                                                    for n, d in files]}


@pytest.mark.parametrize("name", ["../../../etc/passwd", "/etc/passwd", "internal/../../x", "other/file", "internal",
                                  "internal\\..\\x"])
def test_restore_refuses_paths_outside_the_save_folders(game, name):
    a, anchor, *_ = game
    data = evil_archive([reg(name, b"x")], good_manifest((name, b"x")))
    reply = restore(a, data)
    assert not reply["ok"]
    assert not (anchor / "saves-before-restore.tar.gz").exists()  # nothing touched


@pytest.mark.parametrize("kind", [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE])
def test_restore_refuses_links_and_devices(game, kind):
    a, *_ = game
    ti = tarfile.TarInfo("internal/files/x")
    ti.type, ti.linkname = kind, "/etc/passwd"
    reply = restore(a, evil_archive([(ti, None)], good_manifest()))
    assert not reply["ok"] and "link or special" in reply["error"]


def test_restore_refuses_writing_through_a_link_on_the_frame(game):
    a, anchor, internal, external = game
    os.symlink(str(anchor.parent), internal / "lib")
    data = evil_archive([reg("internal/lib/evil.txt", b"x")], good_manifest(("internal/lib/evil.txt", b"x")))
    reply = restore(a, data)
    assert not reply["ok"] and "link" in reply["error"]
    assert not (anchor.parent / "evil.txt").exists()


def test_restore_refuses_damaged_foreign_or_manifestless_archives(game):
    a, *_ = game
    good = ("internal/files/s.sav", b"save")
    assert "no manifest" in restore(a, evil_archive([reg(*good)]))["error"]
    bad_hash = good_manifest(good)
    bad_hash["files"][0]["sha256"] = "0" * 64
    assert "doesn't match" in restore(a, evil_archive([reg(*good)], bad_hash))["error"]
    other = good_manifest(good)
    other["package"] = "com.other.game"
    assert "com.other.game" in restore(a, evil_archive([reg(*good)], other))["error"]
    assert not restore(a, b"not a tar at all")["ok"]
    whole, _ = backup_bytes(a)
    assert not restore(a, whole[: len(whole) // 2])["ok"]  # cut off mid-stream


def test_restore_refuses_oversize_members(game, monkeypatch):
    a, *_ = game
    monkeypatch.setattr(a, "SAVES_RESTORE_MAX_FILE", 3)
    good = ("internal/files/s.sav", b"save")
    reply = restore(a, evil_archive([reg(*good)], good_manifest(good)))
    assert not reply["ok"] and "too big" in reply["error"]


def test_restore_refused_while_running_leaves_everything(game, monkeypatch):
    a, anchor, internal, external = game
    data, _ = backup_bytes(a)
    monkeypatch.setattr(a, "container_running", lambda appid: True)
    reply = restore(a, data)
    assert not reply["ok"] and "running" in reply["error"]
    assert not (anchor / "saves-before-restore.tar.gz").exists()


def test_failed_swap_puts_the_old_saves_back(game, monkeypatch):
    a, anchor, internal, external = game
    data, _ = backup_bytes(a)
    save = external / "files/SaveData.es3"
    save.write_text("current")
    real = a.apply_staged_saves
    calls = []

    def flaky(roots, entries, manifest):
        calls.append(1)
        if len(calls) == 1:
            real(roots, {k: e for k, e in entries.items() if e["kind"] == "dir" or "SaveData" in k[-1]}, manifest)
            raise OSError("disk full")
        return real(roots, entries, manifest)
    monkeypatch.setattr(a, "apply_staged_saves", flaky)
    reply = restore(a, data)
    assert not reply["ok"] and "put back" in reply["error"]
    assert save.read_text() == "current"


def test_saves_info_and_rollback_delete(game):
    a, anchor, *_ = game
    info = a.cmd_saves_info({"package": PKG, "max_file": 1000})
    assert info["files"] >= 4 and info["rollback"] is None and info["running"] is False
    assert [s["path"] for s in info["skipped"]] == ["external/files/big.bundle"]
    data, _ = backup_bytes(a)
    assert restore(a, data)["ok"]
    assert a.cmd_saves_info({"package": PKG})["rollback"]["size"] > 0
    assert a.cmd_saves_rollback_delete({"package": PKG}) == {"removed": True}
    assert a.cmd_saves_info({"package": PKG})["rollback"] is None


def test_namespace_handoff_runs_the_agent_in_podman_unshare(game, monkeypatch):
    a, *_ = game
    seen = {}

    class Done:
        returncode = 0

    def fake_run(cmd, env=None, **kw):
        seen["cmd"], seen["ns"] = cmd, env.get("FRAMEPORT_SAVES_NS")
        return Done()
    monkeypatch.setattr(a, "as_owner", lambda: ["podman", "unshare"])
    monkeypatch.setattr(a.subprocess, "run", fake_run)
    assert a.saves_backup_session({"package": PKG}, io.BytesIO(), io.StringIO()) == 0
    assert seen["cmd"][:2] == ["podman", "unshare"] and seen["cmd"][3:5] == [str(AGENT), "_saves_ns"]
    assert seen["cmd"][5] == "backup" and json.loads(seen["cmd"][6]) == {"package": PKG}
    assert seen["ns"] == "1"
