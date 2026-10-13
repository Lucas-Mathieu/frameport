"""GitHub #161: a newer APK for a game already in the library (game menu "Update with a newer APK…")."""
import zipfile

import pytest

from frameport import pipeline
from frameport.analysis import detect
from frameport.core import library
from frameport.core.models import Recipe
from frameport.patches import base
from frameport.recommend import engine
from frameport.sources import quest_dump


def _apk(path, manifest):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
        z.writestr("classes.dex", b"dex\n035\0")
    return path


def _entry(tmp_path, manifest):
    base.load_all()
    old = _apk(tmp_path / "v1" / "game.apk", manifest)
    data = tmp_path / "v1" / "obb"
    data.mkdir()
    (data / "main.1.com.example.questgame.obb").write_bytes(b"o" * 10)
    a = detect.analyze(old)
    user = Recipe(package=a.package, patches={"frame.adapter": {}, "adapter.scale": {"value": 1.2}}, source="user",
                  status="works")
    library.upsert_game(a.package, title="My Game", title_locked=True, tags=["mine"], apk=str(old),
                        data_dir=str(data), data_files=None, analysis=a.to_dict(),
                        recipe=library.recipe_to_dict(user), suggested=library.recipe_to_dict(engine.suggest(a)))
    return a.package, data


def test_newer_apk_keeps_name_recipe_and_data(tmp_path, quest_manifest, monkeypatch):
    pkg, data = _entry(tmp_path, quest_manifest)
    new = _apk(tmp_path / "v2" / "game.apk", quest_manifest)
    versions = {"v1": 10, "v2": 12}
    monkeypatch.setattr(quest_dump, "_apk_ids", lambda p: (pkg, versions[p.parent.name]))
    res = pipeline.replace_apk(pkg, new)
    g = library.game(pkg)
    assert g["apk"] == str(new) and g["data_dir"] == str(data)  # no data next to the new APK: the old data stays
    assert g["title"] == "My Game" and g["tags"] == ["mine"]
    assert g["recipe"]["patches"]["adapter.scale"] == {"value": 1.2}  # the user's recipe stays
    assert res["older"] is False and (res["old_version"], res["new_version"]) == (10, 12)
    versions["v1"] = 20  # an older build than the one it replaces: the caller warns
    library.upsert_game(pkg, apk=str(tmp_path / "v1" / "game.apk"))
    assert pipeline.replace_apk(pkg, new)["older"] is True


def test_another_app_is_refused(tmp_path, quest_manifest, monkeypatch):
    pkg, _ = _entry(tmp_path, quest_manifest)
    other = _apk(tmp_path / "other" / "x.apk", quest_manifest)
    monkeypatch.setattr(quest_dump, "_apk_ids", lambda p: ("com.other.app", 1) if p == other else (pkg, 1))
    with pytest.raises(pipeline.WrongPackage):
        pipeline.replace_apk(pkg, other)
    assert library.game(pkg)["apk"] != str(other)
