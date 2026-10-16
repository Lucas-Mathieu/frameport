"""Refresh overrides must reach SteamVR before Lepton: its OpenXR runtime cannot switch display modes."""
import ctypes
import json
from types import SimpleNamespace

import pytest
from test_agent import load_agent


def setup(monkeypatch, tmp_path, text="refresh_rate=90.0\n"):
    a = load_agent(monkeypatch, tmp_path)
    base = tmp_path / "game"
    base.mkdir()
    (base / "settings.conf").write_text(text)
    dep = {"base": str(base), "appid": 4200460859, "kind": "quest"}
    calls = []
    vr = SimpleNamespace(refresh_rate=lambda appid, rate: calls.append((appid, rate)),
                         close=lambda: calls.append("close"))
    monkeypatch.setattr(a.SteamVRSettings, "open", classmethod(lambda cls: vr))
    monkeypatch.setattr(a, "container_running", lambda appid: False)
    return a, base, dep, calls


def test_save_writes_both_adapter_configs_and_steamvr(monkeypatch, tmp_path):
    a, base, dep, calls = setup(monkeypatch, tmp_path)
    monkeypatch.setattr(a, "deployment", lambda pkg: dep)
    monkeypatch.setattr(a, "install_controller_models", lambda *args: None)
    result = a.cmd_set_settings({"package": "com.coatsink.alone", "settings": {"refresh_rate": 96.0}})
    assert calls == [(4200460859, 96.0), "close"]
    assert result["refresh_rate"] == {"refresh_rate": 96.0, "app_key": "steam.app.4200460859"}
    assert (base / "settings.conf").read_text() == "refresh_rate=96.0\n"
    assert (base / "lepton-data/external/Android/data/com.coatsink.alone/files/framebridge.conf").read_text() == \
        "refresh_rate=96.0\n"


def test_unavailable_steamvr_retries_on_launch_and_running_game_waits(monkeypatch, tmp_path):
    a, base, dep, calls = setup(monkeypatch, tmp_path)
    open_vr = a.SteamVRSettings.open
    monkeypatch.setattr(a.SteamVRSettings, "open", classmethod(lambda cls: None))
    assert "unavailable" in a.sync_refresh_rate("com.x.y", dep)["skipped"]
    assert not (base / a.REFRESH_RATE_MARKER).exists()
    monkeypatch.setattr(a.SteamVRSettings, "open", open_vr)
    monkeypatch.setattr(a, "container_running", lambda appid: True)
    assert "running" in a.sync_refresh_rate("com.x.y", dep)["skipped"]
    assert calls == []
    monkeypatch.setattr(a, "deployment", lambda pkg: dep)
    a.refresh_rate_worker("com.x.y")
    assert calls == [(4200460859, 90.0), "close"]
    assert (base / a.REFRESH_RATE_MARKER).read_text() == "90.0"


@pytest.mark.parametrize("reset", ["refresh_rate=0\n", "scale=1.0\n"])
def test_games_choice_and_rebuild_without_override_clear_owned_preference(monkeypatch, tmp_path, reset):
    a, base, dep, calls = setup(monkeypatch, tmp_path)
    a.sync_refresh_rate("com.x.y", dep)
    (base / "settings.conf").write_text(reset)
    assert a.sync_refresh_rate("com.x.y", dep)["refresh_rate"] == 0
    assert calls == [(4200460859, 90.0), "close", (4200460859, 0.0), "close"]
    assert not (base / a.REFRESH_RATE_MARKER).exists()


@pytest.mark.parametrize("text", ["scale=1.0\n", "refresh_rate=bad\n", "refresh_rate=nan\n",
                                 "refresh_rate=200\n", "refresh_rate=-90\n"])
def test_unset_or_invalid_rate_does_not_change_steamvr(monkeypatch, tmp_path, text):
    a, base, dep, calls = setup(monkeypatch, tmp_path, text)
    a.sync_refresh_rate("com.x.y", dep)
    assert calls == []
    assert not (base / a.REFRESH_RATE_MARKER).exists()


def test_steamvr_error_keeps_override_pending_and_closes(monkeypatch, tmp_path):
    a, base, dep, calls = setup(monkeypatch, tmp_path)

    def fail(*args):
        raise a.AgentError("write failed")

    vr = SimpleNamespace(refresh_rate=fail, close=lambda: calls.append("close"))
    monkeypatch.setattr(a.SteamVRSettings, "open", classmethod(lambda cls: vr))
    assert a.sync_refresh_rate("com.x.y", dep) == {"skipped": "write failed"}
    assert calls == ["close"] and not (base / a.REFRESH_RATE_MARKER).exists()


@pytest.mark.parametrize("config_sync", [False, True])
@pytest.mark.parametrize("missing_play_log", [False, True])
def test_new_and_upgraded_launchers_select_rate_before_lepton(monkeypatch, tmp_path, config_sync, missing_play_log):
    a, base, dep, calls = setup(monkeypatch, tmp_path)
    anchors = tmp_path / "anchors"
    anchor = anchors / "com.x.y"
    anchor.mkdir(parents=True)
    monkeypatch.setattr(a, "ANCHORS", str(anchors))
    dep["config_sync"] = {"config.xml": {"width": "eye_width"}} if config_sync else {}
    a.write_launcher(str(anchor), str(base), "com.x.y", "Game", dep["appid"], "/lepton", {}, config_sync=config_sync)
    path = anchor / "launch.sh"
    text = path.read_text()
    line = next(line for line in text.splitlines(True) if " _refresh_rate " in line)
    assert "timeout 10s" in line and "9>&- || true" in line
    assert text.index(line) < text.index("\nsetsid ")
    assert a.set_refresh_rate_line(text, "com.x.y") == text
    old = text.replace(line, "")
    if missing_play_log:
        old = "".join(line for line in old.splitlines(True) if "plays.log" not in line)
    path.write_text(old)
    (anchor / "deployment.json").write_text(json.dumps(dep))
    assert a.upgrade_launchers() == ["com.x.y"]
    assert path.read_text().count(" _refresh_rate ") == 1
    assert a.upgrade_launchers() == []
    assert a.set_refresh_rate_line("exec proton run game.exe\n", "com.x.y") == "exec proton run game.exe\n"


def test_settings_function_table_uses_per_app_float_and_removes_only_its_key(monkeypatch, tmp_path):
    a = load_agent(monkeypatch, tmp_path)
    calls = []
    set_float = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float,
                                ctypes.POINTER(ctypes.c_int))(
        lambda section, key, rate, err: calls.append((section, key, rate)))
    remove_key = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int))(
        lambda section, key, err: calls.append((section, key, "remove")))
    vr = a.SteamVRSettings()
    vr.ct = ctypes
    vr.table = (ctypes.c_void_p * 11)()
    vr.table[3] = ctypes.cast(set_float, ctypes.c_void_p)
    vr.table[10] = ctypes.cast(remove_key, ctypes.c_void_p)
    vr.refresh_rate(4200460859, 90)
    vr.refresh_rate(4200460859, 0)
    assert calls == [(b"steam.app.4200460859", b"preferredRefreshRate", 90.0),
                     (b"steam.app.4200460859", b"preferredRefreshRate", "remove")]
