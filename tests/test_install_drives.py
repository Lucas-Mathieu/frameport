"""PC side of games on another drive (GitHub #90) and Linux apps' Desktop Mode entries (GitHub #84)."""
from pathlib import Path

import pytest

from frameport.core import library
from frameport.core.events import Cancelled, Reporter
from frameport.frame.connection import FrameTarget
from frameport.install import drives, installer
from frameport.targets.frame_lepton import FrameLeptonTarget


class FakeFrame:
    """Answers agent commands from a script: {command: reply or [replies] or callable(args)}."""

    def __init__(self, replies):
        self.replies, self.calls = replies, []

    def agent(self, command, timeout=None, **args):
        self.calls.append((command, args))
        r = self.replies[command]
        if callable(r):
            return r(args)
        if isinstance(r, list):
            return r.pop(0) if len(r) > 1 else r[0]
        return r


def test_install_dest_setting():
    assert drives.install_dest() is None
    drives.set_install_dest("/run/media/steamos/SD/FramePort")
    assert drives.install_dest() == "/run/media/steamos/SD/FramePort"
    assert library.setting(drives.SETTING) == "/run/media/steamos/SD/FramePort"
    drives.set_install_dest(None)
    assert drives.install_dest() is None and library.setting(drives.SETTING) == "internal"
    t = FrameLeptonTarget(FrameTarget("frame.local", "steamos", 22))
    drives.set_install_dest("/run/media/steamos/SD/FramePort")
    assert t.install_dest() == "/run/media/steamos/SD/FramePort"
    t.dest = ""  # frameport install --dest internal
    assert t.install_dest() == ""


def test_linux_install_passes_dest_and_desktop_entry(monkeypatch, tmp_path):
    (tmp_path / "app").mkdir()
    (tmp_path / "app" / "tool").write_bytes(b"#!/bin/sh\n")
    monkeypatch.setattr(installer, "upload_steam_art", lambda *a, **k: None)
    monkeypatch.setattr(installer, "_tags", lambda pkg: [])
    frame = FakeFrame({"prepare_linux": {"existing": {"app": {"tool": 10}}, "free_bytes": 1 << 40,
                                         "incoming": "/x/incoming", "base": "/sd/FramePort/linux.tool"},
                       "finalize_linux": {"base": "/sd/FramePort/linux.tool", "appid": 1,
                                          "desktop_entry": {"menu": "/m", "desktop": "/d"}}})
    plan = installer.LinuxPlan("linux.tool", "Tool", tmp_path / "app", "tool", dest="/sd/FramePort",
                               desktop_entry=False)
    rep = Reporter()
    checks = []
    rep.subscribe(lambda ev: ev.kind == "check" and checks.append(ev.data["name"]))
    installer.install_linux(frame, plan, rep)
    assert frame.calls[0] == ("prepare_linux", {"package": "linux.tool", "title": "Tool", "dest": "/sd/FramePort"})
    fin = dict(frame.calls)["finalize_linux"]
    assert fin["dest"] == "/sd/FramePort" and fin["desktop_entry"] is False
    assert "Desktop Mode entry" in checks


def test_move_follows_the_agents_progress():
    status = [{"state": "none"},
              {"state": "running", "package": "com.x.y", "phase": "copying", "done_bytes": 1 << 30,
               "total_bytes": 2 << 30},
              {"state": "running", "package": "com.x.y", "phase": "verifying", "done_bytes": 2 << 30,
               "total_bytes": 2 << 30},
              {"state": "done", "package": "com.x.y", "base": "/sd/FramePort/com.x.y"}]
    frame = FakeFrame({"move_status": status,
                       "move": {"started": True, "from": "/home/a", "to": "/sd/FramePort/com.x.y",
                                "total_bytes": 2 << 30}})
    rep = Reporter()
    seen = []
    rep.subscribe(lambda ev: ev.kind == "progress" and seen.append((round(ev.fraction, 2), ev.message)))
    st = drives.move(frame, "com.x.y", "/sd/FramePort", rep, sleep=lambda s: None)
    assert st["state"] == "done"
    assert ("move", {"package": "com.x.y", "dest": "/sd/FramePort"}) in frame.calls
    assert seen[0] == (0.5, "Copying 1.0/2.0 GiB") and seen[1][1].startswith("Checking the copy")


def test_move_picks_up_a_running_move_and_reports_failures():
    frame = FakeFrame({"move_status": [{"state": "running", "package": "com.x.y", "total_bytes": 0},
                                       {"state": "failed", "error": "the copy doesn't match"}],
                       "move": lambda a: pytest.fail("a running move of the same game isn't started again")})
    with pytest.raises(RuntimeError, match="doesn't match"):
        drives.move(frame, "com.x.y", "internal", Reporter(), sleep=lambda s: None)
    # a retried job (the Frame was lost) whose move finished meanwhile
    from frameport.frame.connection import AgentFailed

    def already(a):
        raise AgentFailed("Game is already there")
    frame = FakeFrame({"move_status": {"state": "done", "package": "com.x.y"}, "move": already})
    assert drives.move(frame, "com.x.y", "internal", Reporter(), sleep=lambda s: None)["state"] == "done"
    # cancelled before the copy started: nothing is asked of the Frame
    frame = FakeFrame({"move_status": {"state": "none"}, "move": lambda a: pytest.fail("started")})
    rep = Reporter()
    rep.cancelled.set()
    with pytest.raises(Cancelled):
        drives.move(frame, "com.x.y", "internal", rep, sleep=lambda s: None)


def test_drive_helpers():
    d_int = {"internal": True, "path": "/home/steamos", "label": "Internal storage", "free_bytes": 12.5 * 2**30}
    d_sd = {"internal": False, "path": "/run/media/steamos/SD", "label": "SD", "free_bytes": None}
    assert drives.drive_text(d_int) == "Internal storage · 12.5 GiB free" and drives.drive_text(d_sd) == "SD"
    on_sd = {"drive": {"internal": False, "path": "/run/media/steamos/SD", "label": "SD"}, "drive_missing": True}
    assert drives.game_drive(on_sd)["missing"] is True and drives.game_drive({"package": "x"}) is None
    assert drives.is_on(on_sd, d_sd) and not drives.is_on(on_sd, d_int)
    assert drives.is_on({"package": "old agent"}, d_int)


def test_target_switches_a_desktop_entry(monkeypatch):
    t = FrameLeptonTarget(FrameTarget("frame.local", "steamos", 22))
    t.frame = FakeFrame({"desktop_entry": lambda a: {"enabled": a["enabled"]}})
    monkeypatch.setattr(t, "connect", lambda: t)
    assert t.set_desktop_entry("linux.tool", False) == {"enabled": False}
    assert t.frame.calls == [("desktop_entry", {"package": "linux.tool", "enabled": False})]


def test_pipeline_passes_the_desktop_entry_choice(monkeypatch):
    from frameport import pipeline

    library.upsert_game("linux.tool", kind="linux", title="Tool", game_dir=str(Path("/apps/tool")), exe="tool",
                        desktop_entry=False, analysis={"extra": {}})
    got = {}

    class T:
        label = "Frame"

        def install_linux(self, *a, **k):
            got.update(k)
            return {}

    monkeypatch.setattr(pipeline, "steam_title", lambda e: "Tool")
    pipeline.install_linux("linux.tool", T(), Reporter(), add_to_library=False)
    assert got["desktop_entry"] is False


SD = {"internal": False, "path": "/run/media/steamos/SD", "install_dir": "/run/media/steamos/SD/FramePort",
      "label": "SD", "free_bytes": 50 * 2**30}
HOME = {"internal": True, "path": "/home", "install_dir": "/home/steamos/Applications/quest-frame",
        "label": "Internal", "free_bytes": 4 * 2**30}


def test_target_drive_new_games_go_to_the_chosen_drive_installed_ones_stay():
    listed = [HOME, SD]
    assert drives.target_drive(None, None, listed) is HOME
    assert drives.target_drive(None, SD["install_dir"], listed) is SD
    assert drives.target_drive(None, "/run/media/steamos/GONE/FramePort", listed) is None  # card not inserted
    on_sd = {"package": "a", "drive": {"internal": False, "path": SD["path"], "label": "SD"}}
    assert drives.target_drive(on_sd, None, listed) is SD
    assert drives.target_drive({"package": "b"}, SD["install_dir"], listed) is HOME  # agent < v63: internal


def test_space_shortfalls_per_drive():
    """GitHub #205: the warning used internal storage's free space even when new games go to the SD card."""
    gib = 2**30
    listed = [HOME, SD]
    assert drives.space_shortfalls([(None, 30 * gib)], SD["install_dir"], listed, 4 * gib) == []
    assert drives.space_shortfalls([(None, 30 * gib)], None, listed, 4 * gib) == [(HOME, 30 * gib, 4 * gib)]
    assert drives.space_shortfalls([(None, 30 * gib), (None, 30 * gib)], SD["install_dir"], listed, 4 * gib) \
        == [(SD, 60 * gib, 50 * gib)]
    # an update of a game already on internal storage still counts there
    inside = {"package": "x", "drive": {"internal": True, "path": "/home", "label": "Internal"}}
    assert drives.space_shortfalls([(inside, 5 * gib), (None, 30 * gib)], SD["install_dir"], listed, 4 * gib) \
        == [(HOME, 5 * gib, 4 * gib)]
    # the chosen card isn't there: the agent refuses that install itself
    assert drives.space_shortfalls([(None, 30 * gib)], "/run/media/steamos/GONE/FramePort", listed, 4 * gib) == []


def test_space_shortfalls_without_a_drive_list():
    gib = 2**30
    assert drives.space_shortfalls([(None, 5 * gib)], None, None, 4 * gib)[0][1:] == (5 * gib, 4 * gib)
    assert drives.space_shortfalls([(None, 5 * gib)], "/run/media/steamos/SD/FramePort", None, 4 * gib) == []
    on_sd = {"package": "a", "drive": {"internal": False, "path": "/run/media/steamos/SD", "label": "SD"}}
    assert drives.space_shortfalls([(on_sd, 5 * gib)], None, None, 4 * gib) == []
    assert drives.space_shortfalls([(None, 5 * gib)], None, [], None) == []  # nothing known
