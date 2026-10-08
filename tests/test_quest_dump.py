"""Finding Quest games and their data (OBB) folders on disk (sources/quest_dump.py)."""
import zipfile
from pathlib import Path

import pytest

from frameport.sources import quest_dump


@pytest.fixture(autouse=True)
def package_from_file_name(monkeypatch):
    monkeypatch.setattr(quest_dump, "_package_of", lambda apk: Path(apk).stem.split("_")[0])


def apk(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", b"x")
    return path


def obb(folder: Path, name: str = "main.1.com.x.game.obb") -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / name).write_bytes(b"o")
    return folder


def test_existing_layouts(tmp_path):
    a = tmp_path / "Game A v1 -TAG"
    apk(a / "com.x.game.apk")
    obb(a / "com.x.game")
    g = quest_dump.from_path(a)
    assert (g.name, g.data_dir, g.origin) == ("Game A v1", a / "com.x.game", a)
    b = tmp_path / "patched"
    apk(b / "com.x.game.apk")
    obb(b / "obb")
    assert quest_dump.from_path(b).data_dir == b / "obb"
    assert quest_dump.from_path(b / "com.x.game.apk").data_dir == b / "obb"  # a single APK file
    c = tmp_path / "no data"
    apk(c / "com.x.game.apk")
    assert quest_dump.from_path(c).data_dir is None


@pytest.mark.parametrize("data", ["obb/com.x.game", "OBB/com.x.game", "obbs/com.x.game", "Android/obb/com.x.game",
                                  "backup/com.x.game"])
def test_package_folder_below_the_apk(tmp_path, data):
    """The data folder is the one holding the .obb files (its contents go to Android/obb/<package>/)."""
    game = tmp_path / "Game"
    apk(game / "com.x.game.apk")
    obb(game / data)
    found = quest_dump.scan(tmp_path)
    assert len(found) == 1 and found[0].data_dir == game / data and found[0].name == "Game"


@pytest.mark.parametrize("apk_folder", ["apk", "APKs"])
def test_sidequest_style_backup_is_one_game(tmp_path, apk_folder):
    game = tmp_path / "Backups" / "Some Game"
    apk(game / apk_folder / "com.x.game.apk")
    obb(game / "obb" / "com.x.game")
    found = quest_dump.scan(tmp_path / "Backups")
    assert len(found) == 1
    g = found[0]
    assert (g.name, g.origin, g.data_dir) == ("Some Game", game, game / "obb" / "com.x.game")
    assert quest_dump.from_path(g.apk).data_dir == game / "obb" / "com.x.game"  # the APK file alone too


def test_other_games_obb_is_never_taken(tmp_path):
    """A neighbouring folder with its own APK is another game (e.g. another version): its data stays its own."""
    v1, v2 = tmp_path / "Game v1", tmp_path / "Game v2"
    apk(v1 / "com.x.game.apk")
    apk(v2 / "com.x.game.apk")
    obb(v2 / "data" / "com.x.game")
    found = {g.origin.name: g for g in quest_dump.scan(tmp_path)}
    assert found["Game v1"].data_dir is None
    assert found["Game v2"].data_dir == v2 / "data" / "com.x.game"


def test_package_folder_without_obb_files_is_ignored_when_searched(tmp_path):
    game = tmp_path / "Game"
    apk(game / "apk" / "com.x.game.apk")
    (game / "saves" / "com.x.game").mkdir(parents=True)
    (game / "saves" / "com.x.game" / "save.dat").write_bytes(b"s")
    assert quest_dump.from_path(game / "apk").data_dir is None


# ------------------------------------------------------------------ a game that expects an OBB (GitHub #85)
def test_expects_obb_from_unreals_manifest_flag():
    from frameport.analysis.detect import expects_obb

    assert expects_obb({"com.epicgames.ue4.GameActivity.bHasOBBFiles": True})
    assert expects_obb({"com.epicgames.unreal.GameActivity.bHasOBBFiles": "true"})  # UE5's name
    assert not expects_obb({"com.epicgames.ue4.GameActivity.bHasOBBFiles": False})
    assert not expects_obb({"com.epicgames.ue4.GameActivity.bVerifyOBBOnStartUp": True})


def test_analysis_reads_the_obb_flag(tmp_path):
    from conftest import build_axml

    from frameport.analysis.detect import analyze

    manifest = build_axml([
        ("start", "manifest", [("package", "str", "com.x.game")]),
        ("start", "application", []),
        ("start", "meta-data", [("name", "str", "com.epicgames.ue4.GameActivity.bHasOBBFiles"),
                                ("value", "bool", True)]),
        ("end", "meta-data"),
        ("end", "application"),
        ("end", "manifest"),
    ])
    p = tmp_path / "game.apk"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
    assert analyze(p).extra["expects_obb"] is True


def _entry(**kw):
    e = {"package": "com.x.game", "analysis": {"extra": {"expects_obb": True}}, "data_dir": None, "data_bytes": 0}
    e.update(kw)
    return e


def test_missing_obb_is_flagged_before_install_and_in_launch_tests():
    from frameport import pipeline
    from frameport.validate.triage import TriageResult, add_missing_obb

    assert pipeline.missing_obb(_entry())
    assert not pipeline.missing_obb(_entry(data_dir="/games/x/com.x.game", data_bytes=10))
    assert not pipeline.missing_obb(_entry(data_dir="/games/x/com.x.game", data_bytes=None))  # older entries
    assert not pipeline.missing_obb(_entry(analysis={"extra": {"expects_obb": False}}))
    assert not pipeline.missing_obb(_entry(kind="rift"))
    from test_patches import _analysis

    analysis = _analysis(package="com.x.game", engine="Unreal", extra={"size": 1, "expects_obb": True}).to_dict()
    [note] = pipeline.analysis_warnings(_entry(analysis=analysis))
    assert "folder named com.x.game" in note and "hangs at start" in note
    # the launch test: the game logs nothing about it, so the finding comes from the library
    r = add_missing_obb(TriageResult("RUNNING", "Lepton started"))
    assert [f.id for f in r.findings] == ["missing-obb"] and r.verdict == "fail"
    running = TriageResult("RUNNING", "Submitting frames", fps=72.0)  # an OBB from an earlier install is there
    assert not add_missing_obb(running).findings and running.verdict == "pass"
