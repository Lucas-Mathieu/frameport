"""GitHub discussion #351: the library folders list (Settings → Library folders, `frameport folders`) and scans that
find games of the library at a new place (moved: relinked in place; another version; another copy)."""
import shutil
import zipfile

from typer.testing import CliRunner

from frameport import cli, pipeline
from frameport.artwork import fetch as artwork
from frameport.core import folders, library
from frameport.core.events import Reporter
from frameport.patches import base
from frameport.sources import quest_dump

PKG = "com.example.questgame"


# ------------------------------------------------------------------------------------------ the folder list
def test_folder_list_add_remove_status(tmp_path):
    a, b = tmp_path / "A", tmp_path / "B"
    a.mkdir()
    b.mkdir()
    folders.remember(a)
    folders.remember(b)
    folders.remember(a)  # scanned again: once, moved to the end
    assert folders.roots() == [str(b), str(a)]
    assert folders.add(b) == [str(b), str(a)]  # already there: unchanged
    library.upsert_game("g1", apk=str(a / "x" / "g.apk"))
    library.upsert_game("g2", kind="rift", game_dir=str(a / "Game"))
    library.upsert_game("g3", apk=str(tmp_path / "elsewhere.apk"))
    shutil.rmtree(b)
    st = {s.path: s for s in folders.status()}
    assert (st[str(a)].exists, st[str(a)].games) == (True, 2)
    assert (st[str(b)].exists, st[str(b)].games) == (False, 0)
    assert folders.rescan_roots() == [str(a)]  # a missing folder isn't scanned
    assert folders.remove(str(a)) is True and folders.remove(str(a)) is False
    assert library.game("g1") and library.game("g2")  # its games stay
    try:
        folders.add(tmp_path / "nope")
        raise AssertionError("a missing folder can't be added")
    except ValueError:
        pass


def test_rescan_roots_fallback_only_before_anything_was_remembered(tmp_path):
    games = tmp_path / "Games"
    (games / "One").mkdir(parents=True)
    library.upsert_game("g1", apk=str(games / "One" / "g.apk"), origin=str(games / "One"))
    assert folders.rescan_roots() == [str(games)]  # an older library: where its games came from
    folders.remember(games)
    folders.remember(games / "One")  # inside another one: scanned with it
    assert folders.rescan_roots() == [str(games)]
    folders.remove(games)
    folders.remove(games / "One")
    assert folders.rescan_roots() == []  # the user removed them all: no guessing them back


def test_cli_folders(tmp_path):
    d = tmp_path / "Games"
    d.mkdir()
    runner = CliRunner()
    res = runner.invoke(cli.app, ["folders", "--add", str(d)])
    assert res.exit_code == 0 and str(d) in res.output and "ok" in res.output
    assert folders.roots() == [str(d)]
    res = runner.invoke(cli.app, ["folders", "--json"])
    assert '"exists": true' in res.output
    res = runner.invoke(cli.app, ["folders", "--remove", str(d)])
    assert res.exit_code == 0 and folders.roots() == []
    assert runner.invoke(cli.app, ["folders", "--remove", str(d)]).exit_code == 1
    assert runner.invoke(cli.app, ["folders", "--add", str(tmp_path / "missing")]).exit_code != 0
    assert "no folders to rescan" in runner.invoke(cli.app, ["rescan"]).output


# ------------------------------------------------------------------------------------------ Quest games that moved
def _apk(path, manifest, extra=b""):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
        z.writestr("classes.dex", b"dex\n035\0" + extra)
    return path


def _game(root, manifest, extra=b""):
    apk = _apk(root / "Quest Game" / "game.apk", manifest, extra)
    obb = root / "Quest Game" / PKG
    obb.mkdir()
    (obb / f"main.1.{PKG}.obb").write_bytes(b"o" * 10)
    return apk


def _setup(tmp_path, monkeypatch, manifest):
    base.load_all()
    monkeypatch.setattr(quest_dump, "_apk_ids", lambda p: (PKG, 1))
    monkeypatch.setattr(pipeline.rift_dump, "scan", lambda *a, **k: [])
    _game(tmp_path / "Old", manifest)
    [g] = pipeline.add_path(tmp_path / "Old")
    r = library.recipe_from_dict(g["recipe"])
    r.source, r.status = "user", "works"
    r.patches["adapter.scale"] = {"value": 1.2}
    library.upsert_game(PKG, title="My Game", title_locked=True, tags=["mine"], recipe=library.recipe_to_dict(r),
                        status="works", build={"ok": True, "sha256": "b"}, installs={"Frame": {"time": 1}})
    art = artwork.artwork_dir(PKG)
    (art / artwork.PICKED).write_text("custom")
    return g


def _logs():
    logs = []
    rep = Reporter([lambda e: logs.append(e.message) if e.kind == "log" else None])
    return rep, logs


def _kept_user_fields(g):
    assert (g["title"], g["title_locked"], g["tags"]) == ("My Game", True, ["mine"])
    assert g["recipe"]["source"] == "user" and g["recipe"]["patches"]["adapter.scale"] == {"value": 1.2}
    assert g["status"] == "works" and g["build"] == {"ok": True, "sha256": "b"} and g["installs"]
    assert (artwork.artwork_dir(PKG) / artwork.PICKED).is_file()


def test_rescan_relinks_a_moved_quest_game(tmp_path, monkeypatch, quest_manifest):
    _setup(tmp_path, monkeypatch, quest_manifest)
    shutil.move(tmp_path / "Old", tmp_path / "New")  # e.g. another drive
    rep, logs = _logs()
    report = pipeline.ScanReport()
    assert pipeline.add_path(tmp_path / "New", rep, only_new=True, report=report) == []
    g = library.game(PKG)
    assert [i["package"] for i in report.moved] == [PKG]
    new = tmp_path / "New" / "Quest Game"
    assert g["apk"] == str(new / "game.apk") and g["data_dir"] == str(new / PKG) and g["origin"] == str(new)
    assert g["data_manifest"] == {f"main.1.{PKG}.obb": 10}
    _kept_user_fields(g)
    assert any("aren't at" in m and "drive" in m for m in logs)  # the old place is gone (maybe unplugged)
    assert any("Updated the location of 1 game" in m for m in logs)
    pipeline.check_source(g)  # buildable again


def test_full_scan_keeps_a_user_recipe_and_refreshes_a_derived_one(tmp_path, monkeypatch, quest_manifest):
    g0 = _setup(tmp_path, monkeypatch, quest_manifest)
    [g] = pipeline.add_path(tmp_path / "Old")  # scanned again, same place
    _kept_user_fields(g)
    assert g["suggested"] == g0["suggested"]
    stale = dict(g0["recipe"], patches={}, source="heuristics")
    library.upsert_game(PKG, recipe=stale)
    [g] = pipeline.add_path(tmp_path / "Old")
    assert g["recipe"] == g["suggested"]  # not the user's: follows the analysis


def test_another_version_isnt_swapped_silently(tmp_path, monkeypatch, quest_manifest):
    _setup(tmp_path, monkeypatch, quest_manifest)
    shutil.rmtree(tmp_path / "Old")
    newer = _game(tmp_path / "New", quest_manifest, extra=b"v2")  # other bytes: another build
    report = pipeline.ScanReport()
    pipeline.add_path(tmp_path / "New", only_new=True, report=report)  # Rescan folders: the user decides
    assert [i["package"] for i in report.other_version] == [PKG] and not report.moved
    assert library.game(PKG)["apk"] == str(tmp_path / "Old" / "Quest Game" / "game.apk")
    report = pipeline.ScanReport()
    assert pipeline.add_path(tmp_path / "New", report=report) == []  # Scan a folder…: like Update with a newer APK…
    g = library.game(PKG)
    assert [i["package"] for i in report.updated] == [PKG]
    assert g["apk"] == str(newer) and g["source_sha256"] == pipeline.builder.sha256(newer)
    assert (g["title"], g["tags"], g["recipe"]["source"]) == ("My Game", ["mine"], "user")


def test_a_second_copy_leaves_the_existing_one(tmp_path, monkeypatch, quest_manifest):
    _setup(tmp_path, monkeypatch, quest_manifest)
    _game(tmp_path / "Copy", quest_manifest)
    for only_new in (True, False):
        report = pipeline.ScanReport()
        assert pipeline.add_path(tmp_path / "Copy", only_new=only_new, report=report) == []
        assert [i["package"] for i in report.other_copy] == [PKG]
        g = library.game(PKG)
        assert g["apk"] == str(tmp_path / "Old" / "Quest Game" / "game.apk")
        _kept_user_fields(g)


def test_same_build_without_a_checksum_uses_the_version_name(tmp_path, monkeypatch):
    apk = tmp_path / "x.apk"
    apk.write_bytes(b"x")
    monkeypatch.setattr(quest_dump, "version_name", lambda p: "1.2")
    assert pipeline.same_quest_build({"analysis": {"version": "1.2"}}, apk)
    assert not pipeline.same_quest_build({"analysis": {"version": "1.1"}}, apk)
    assert not pipeline.same_quest_build({"source_sha256": "0" * 64, "analysis": {"version": "1.2"}}, apk)


def test_scan_summary_lines():
    from frameport.ui.app import scan_report_lines

    report = pipeline.ScanReport()
    assert scan_report_lines(report) == []
    report.note("moved", {"package": "a", "title": "A"}, "/x")
    report.note("moved", {"package": "b", "title": "B"}, "/y")
    report.note("other_version", {"package": "c"}, "/z")
    lines = scan_report_lines(report)
    assert lines[0] == "Updated the location of 2 games" and "another version" in lines[1]


# ------------------------------------------------------------------------------------------ PC VR games that moved
def _rift(tmp_path):
    from test_rift import unity_game

    unity_game(tmp_path / "Old")
    [g] = pipeline.add_path(tmp_path / "Old")
    pkg = g["package"]
    r = library.recipe_from_dict(g["recipe"])
    r.source = "user"
    r.patches["pcvr.no_crash_reporter"] = {}
    pipeline.set_recipe(pkg, r)
    library.upsert_game(pkg, title="My Space", title_locked=True, tags=["mine"], exe_confirmed=True)
    return pkg


def _rift_kept(g):
    assert (g["title"], g["tags"], g["exe"]) == ("My Space", ["mine"], "Space Game.exe")
    assert g["recipe"]["source"] == "user" and "pcvr.no_crash_reporter" in g["recipe"]["patches"]


def test_rescan_relinks_a_moved_rift_game(tmp_path):
    pkg = _rift(tmp_path)
    (tmp_path / "New").mkdir()
    shutil.move(tmp_path / "Old" / "Space Game", tmp_path / "New" / "Space Game")
    report = pipeline.ScanReport()
    assert pipeline.add_path(tmp_path / "New", only_new=True, report=report) == []
    g = library.game(pkg)
    new = tmp_path / "New" / "Space Game"
    assert [i["package"] for i in report.moved] == [pkg]
    assert g["game_dir"] == str(new) and g["analysis"]["extra"]["folder"] == str(new)
    _rift_kept(g)
    assert len([x for x in library.games() if x.get("kind") == "rift"]) == 1  # no second entry


def test_a_renamed_rift_folder_is_found_by_its_program(tmp_path):
    pkg = _rift(tmp_path)
    (tmp_path / "New").mkdir()
    shutil.move(tmp_path / "Old" / "Space Game", tmp_path / "New" / "Space Game (repack)")
    report = pipeline.ScanReport()
    pipeline.add_path(tmp_path / "New", only_new=True, report=report)
    assert [i["package"] for i in report.moved] == [pkg]
    assert library.game(pkg)["game_dir"] == str(tmp_path / "New" / "Space Game (repack)")
    assert len([x for x in library.games() if x.get("kind") == "rift"]) == 1


def test_rift_copy_and_version(tmp_path):
    pkg = _rift(tmp_path)
    shutil.copytree(tmp_path / "Old" / "Space Game", tmp_path / "Copy" / "Space Game")
    report = pipeline.ScanReport()
    pipeline.add_path(tmp_path / "Copy", report=report)  # the library's folder is still there: it stays
    assert [i["package"] for i in report.other_copy] == [pkg]
    assert library.game(pkg)["game_dir"] == str(tmp_path / "Old" / "Space Game")
    shutil.rmtree(tmp_path / "Old")
    exe = tmp_path / "Copy" / "Space Game" / "Space Game.exe"
    exe.write_bytes(exe.read_bytes() + b"\0" * 512)  # another build
    report = pipeline.ScanReport()
    pipeline.add_path(tmp_path / "Copy", only_new=True, report=report)
    assert [i["package"] for i in report.other_version] == [pkg]
    assert library.game(pkg)["game_dir"] == str(tmp_path / "Old" / "Space Game")
    report = pipeline.ScanReport()
    pipeline.add_path(tmp_path / "Copy", report=report)
    g = library.game(pkg)
    assert [i["package"] for i in report.updated] == [pkg] and g["source_id"]
    assert g["game_dir"] == str(tmp_path / "Copy" / "Space Game")
    _rift_kept(g)
