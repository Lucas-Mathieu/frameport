"""GitHub #161: a newer version of a Linux app or a PC VR game already in the library (game menu "Update with a newer
version…", CLI `frameport new-version`)."""
import os
import time

import pytest
from test_install_drives import FakeFrame
from test_linux_apps import elf
from test_rift import unity_game

from frameport import pipeline
from frameport.analysis import linux
from frameport.core import library
from frameport.core.events import Reporter
from frameport.install import installer
from frameport.ui import components as C


def _appimage(path, extra=b""):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(elf(linux.EM_AARCH64, appimage=True, extra=extra))
    return path


def _frame(package, recipe=None):
    return {"installed": [{"package": package, "sha256": None, "recipe": recipe}]}


# ------------------------------------------------------------------------------------------ Linux apps
def test_newer_appimage_keeps_name_art_and_switches(tmp_path):
    old = _appimage(tmp_path / "v1" / "Venera-Prime-2.4.2-aarch64.AppImage")
    pkg = pipeline.add_linux_app(old)["package"]
    library.upsert_game(pkg, title="My Venera", title_locked=True, tags=["mine"], desktop_entry=False,
                        desktop_icon=False, vr_overlay=True)
    assert C.install_state(library.game(pkg), _frame(pkg)) == "installed"  # never updated: no source id needed
    new = _appimage(tmp_path / "v2" / "Venera-Prime-2.5.0-aarch64.AppImage", extra=b"newer")
    res = pipeline.replace_linux_app(pkg, new)
    g = library.game(pkg)
    assert g["package"] == pkg and g["exe"] == new.name and g["game_dir"] == str(new.parent)
    assert g["analysis"]["extra"]["source"] == str(new) and g["analysis"]["extra"]["files"] == [new.name]
    assert g["data_bytes"] == new.stat().st_size
    assert (g["title"], g["title_locked"], g["tags"]) == ("My Venera", True, ["mine"])
    assert (g["desktop_entry"], g["desktop_icon"], g["vr_overlay"]) == (False, False, True)
    assert g["replaced_source"] == str(old) and g["source_id"] and res["different"] is False
    # the Frame's copy (installed before) is outdated until an install records this version
    assert C.install_state(g, _frame(pkg)) == "outdated"
    assert C.install_state(g, _frame(pkg, {"source_id": "older"})) == "outdated"
    assert C.install_state(g, _frame(pkg, {"source_id": g["source_id"]})) == "installed"


def test_newer_linux_folder_keeps_the_chosen_program(tmp_path):
    def app(root, size):
        (root / "bin").mkdir(parents=True)
        (root / "Tool").write_bytes(elf(linux.EM_AARCH64, extra=b"\0" * size))  # bigger: FramePort's pick
        (root / "bin" / "tool-vr").write_bytes(elf(linux.EM_AARCH64))
        return root
    pkg = pipeline.add_linux_app(app(tmp_path / "v1" / "Tool", 4096), exe="bin/tool-vr")["package"]
    assert library.game(pkg)["exe"] == "bin/tool-vr"
    pipeline.replace_linux_app(pkg, app(tmp_path / "v2" / "Tool", 8192))
    g = library.game(pkg)
    assert g["exe"] == "bin/tool-vr" and g["game_dir"] == str(tmp_path / "v2" / "Tool")
    # the program isn't there any more: FramePort's pick
    (tmp_path / "v3" / "Tool").mkdir(parents=True)
    (tmp_path / "v3" / "Tool" / "Tool").write_bytes(elf(linux.EM_AARCH64))
    pipeline.replace_linux_app(pkg, tmp_path / "v3" / "Tool")
    assert library.game(pkg)["exe"] == "Tool"


def test_another_linux_app_is_added_with_a_warning(tmp_path):
    pkg = pipeline.add_linux_app(_appimage(tmp_path / "Venera-Prime-1.0-aarch64.AppImage"))["package"]
    res = pipeline.replace_linux_app(pkg, _appimage(tmp_path / "Blender-4.2-aarch64.AppImage"))
    assert res["different"] and res["name"] == "Blender" and library.game(pkg)["exe"].startswith("Blender")
    assert not library.game("linux.blender")  # no second entry: the user's pick is the newer version
    with pytest.raises(ValueError):
        pipeline.replace_linux_app(pkg, tmp_path / "missing.AppImage")


def test_only_the_right_kind(tmp_path):
    pkg = pipeline.add_linux_app(_appimage(tmp_path / "Tool-aarch64.AppImage"))["package"]
    with pytest.raises(ValueError):
        pipeline.replace_rift_game(pkg, tmp_path)
    library.upsert_game("com.x.quest", title="Quest", apk="/x.apk")
    with pytest.raises(ValueError):
        pipeline.replace_linux_app("com.x.quest", tmp_path / "Tool-aarch64.AppImage")


def test_linux_update_sends_changed_files_of_the_same_size(tmp_path, monkeypatch):
    (tmp_path / "app").mkdir()
    for name in ("tool", "data.txt"):
        (tmp_path / "app" / name).write_bytes(b"x" * 10)
    old = time.time() - 3600
    os.utime(tmp_path / "app" / "data.txt", (old, old))
    monkeypatch.setattr(installer, "upload_steam_art", lambda *a, **k: None)
    monkeypatch.setattr(installer, "apply_app_icon", lambda *a, **k: None)
    monkeypatch.setattr(installer, "_tags", lambda pkg: [])
    sent = []
    monkeypatch.setattr(installer, "upload_files", lambda xfer, files, *a: sent.extend(f[1] for f in files))
    monkeypatch.setattr(installer, "transfer_link", lambda *a: _Null())
    frame = FakeFrame({"prepare_linux": {"existing": {"app": {"tool": 10, "data.txt": 10}}, "free_bytes": 1 << 40,
                                         "incoming": "/x/incoming", "base": "/b"},
                       "finalize_linux": {"base": "/b", "appid": 1}})
    plan = installer.LinuxPlan("linux.tool", "Tool", tmp_path / "app", "tool")
    installer.install_linux(frame, plan, Reporter())
    assert sent == [] and dict(frame.calls)["finalize_linux"]["recipe"] is None  # same sizes: nothing sent
    # (st_ctime can't be set: the file's own ctime is now) a newer version: files changed since the last install
    plan = installer.LinuxPlan("linux.tool", "Tool", tmp_path / "app", "tool", source_id="v2",
                               changed_since=time.time() + 60)
    installer.install_linux(frame, plan, Reporter())
    assert sent == []
    plan.changed_since = time.time() - 60
    installer.install_linux(frame, plan, Reporter())
    assert sorted(sent) == ["app/data.txt", "app/tool"]
    assert frame.calls[-1][1]["recipe"] == {"source_id": "v2"}


class _Null:
    def __enter__(self):
        return None

    def __exit__(self, *a):
        return False


def test_install_records_the_version(tmp_path, monkeypatch):
    pkg = pipeline.add_linux_app(_appimage(tmp_path / "v1" / "Tool-aarch64.AppImage"))["package"]
    got = {}

    class T:
        label = "Frame"

        def install_linux(self, *a, **k):
            got.clear()
            got.update(k)
            return {}
    monkeypatch.setattr(pipeline, "steam_title", lambda e: "Tool")
    pipeline.install_linux(pkg, T(), Reporter(), add_to_library=False)
    assert "source_id" not in got  # never updated: installs as before
    pipeline.replace_linux_app(pkg, _appimage(tmp_path / "v2" / "Tool-aarch64.AppImage"))
    sid = library.game(pkg)["source_id"]
    pipeline.install_linux(pkg, T(), Reporter(), add_to_library=False)
    assert got["source_id"] == sid and 0 < got["changed_since"] <= time.time()  # since the last install there
    assert library.game(pkg)["installs"]["Frame"]["source_id"] == sid
    pipeline.install_linux(pkg, T(), Reporter(), add_to_library=False)
    assert got["changed_since"] is None  # this version is there already: resumes as usual


# ------------------------------------------------------------------------------------------ PC VR (Rift) games
def _rift(tmp_path):
    first = unity_game(tmp_path / "v1")
    [g] = pipeline.add_path(tmp_path / "v1")
    pkg = g["package"]
    r = library.recipe_from_dict(g["recipe"])
    r.source = "user"
    r.patches["pcvr.no_crash_reporter"] = {}
    pipeline.set_recipe(pkg, r)
    library.upsert_game(pkg, title="My Space", tags=["mine"], exe_confirmed=True,
                        build={"ok": True, "sha256": "old", "kind": "rift"})
    return pkg, first


def test_newer_rift_folder_keeps_name_recipe_and_program(tmp_path):
    pkg, first = _rift(tmp_path)
    new = unity_game(tmp_path / "v2")
    res = pipeline.replace_rift_game(pkg, new)
    g = library.game(pkg)
    assert g["game_dir"] == str(new) and g["exe"] == "Space Game.exe" and g["exe_confirmed"] is True
    assert (g["title"], g["tags"]) == ("My Space", ["mine"])
    assert "pcvr.no_crash_reporter" in g["recipe"]["patches"] and g["recipe"]["source"] == "user"
    assert g["build"] is None and g["replaced_dirs"] == [str(first)] and not res["different"]
    from frameport.patches.pcvr import game_args

    dep = {"package": pkg, "sha256": "old", "recipe": {"patches": sorted(g["recipe"]["patches"])},
           "game_args": game_args(library.recipe_from_dict(g["recipe"]))}
    assert C.install_state(g, {"installed": [dep]}) == "outdated"
    assert C.pc_outdated(g, dep)
    dep["recipe"]["source_id"] = g["source_id"]
    assert C.install_state(g, {"installed": [dep]}) == "installed" and not C.pc_outdated(g, dep)
    # a rescan of the folders doesn't bring the old copy back
    assert pipeline.add_path(tmp_path / "v1") == [] and library.game(pkg)["game_dir"] == str(new)


def test_newer_rift_folder_without_the_program_picks_again(tmp_path):
    pkg, _ = _rift(tmp_path)
    new = unity_game(tmp_path / "v2", name="Space Game VR")
    res = pipeline.replace_rift_game(pkg, new)
    assert res["exe_changed"] and library.game(pkg)["exe"] == "Space Game VR.exe"
    with pytest.raises(ValueError):
        pipeline.replace_rift_game(pkg, tmp_path / "nothing")
    other = unity_game(tmp_path / "other", name="Moon Game")
    pipeline.add_path(tmp_path / "other")
    with pytest.raises(ValueError, match="already in the library"):
        pipeline.replace_rift_game(pkg, other)


def test_rift_update_resends_changed_game_files(tmp_path, monkeypatch):
    game = unity_game(tmp_path)
    monkeypatch.setattr(installer, "upload_steam_art", lambda *a, **k: None)
    monkeypatch.setattr(installer, "_tags", lambda pkg: [])
    monkeypatch.setattr(installer, "transfer_link", lambda *a: _Null())
    sent = []
    monkeypatch.setattr(installer, "upload_files", lambda xfer, files, *a: sent.extend(f[1] for f in files))
    monkeypatch.setattr("frameport.artwork.sources.has_art", lambda pkg: True)
    manifest = installer.local_data_manifest(game)
    frame = FakeFrame({"prepare_pcvr": {"proton": {"display_name": "Proton"}, "openxr": None,
                                        "existing": {"game": dict(manifest)}, "free_bytes": 1 << 40,
                                        "incoming": "/i", "base": "/b"},
                       "finalize_pcvr": {"base": "/b", "proton": "p", "appid": 1}})
    from frameport.core.models import Recipe

    plan = installer.PcvrPlan("rift.space_game", "Space", game, "Space Game.exe", Recipe(package="rift.space_game"),
                              None, source_id="v2", changed_since=time.time() - 60)
    installer.install_pcvr(frame, plan, Reporter())
    assert sorted(sent) == sorted(f"game/{rel}" for rel in manifest)
    assert frame.calls[-1][1]["recipe"]["source_id"] == "v2"


# ------------------------------------------------------------------------------------------ CLI
def test_cli_new_version(tmp_path):
    from typer.testing import CliRunner

    from frameport import cli

    pkg = pipeline.add_linux_app(_appimage(tmp_path / "v1" / "Tool-1.0-aarch64.AppImage"))["package"]
    new = _appimage(tmp_path / "v2" / "Tool-1.1-aarch64.AppImage")
    out = CliRunner().invoke(cli.app, ["new-version", pkg, str(new)])
    assert out.exit_code == 0, out.output
    assert library.game(pkg)["exe"] == new.name and "program Tool-1.1-aarch64.AppImage" in out.output
    (tmp_path / "empty").mkdir()
    bad = CliRunner().invoke(cli.app, ["new-version", pkg, str(tmp_path / "empty")])  # no program in it
    assert bad.exit_code == 2
