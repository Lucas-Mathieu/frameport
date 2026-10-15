"""GitHub #294: side notes about a game that isn't in the library (Play on the Steam Frame tab) must not create a
library entry without a recipe, and such entries already in a library are dropped."""
import json

from frameport.core import library


def test_set_fields_never_creates_an_entry(monkeypatch, tmp_path):
    monkeypatch.setattr(library, "_path", lambda: tmp_path / "library.json")
    assert library.set_fields("com.only.on.frame", last_played=1.0) is None
    assert library.game("com.only.on.frame") is None
    library.upsert_game("com.x.y", title="X", analysis={"package": "com.x.y"}, recipe={"package": "com.x.y"})
    assert library.set_fields("com.x.y", last_played=2.0)["last_played"] == 2.0
    assert library.game("com.x.y")["title"] == "X"


def test_stub_entries_are_dropped_once(monkeypatch, tmp_path):
    path = tmp_path / "library.json"
    monkeypatch.setattr(library, "_path", lambda: path)
    real = {"package": "com.real", "analysis": {"package": "com.real"}, "recipe": {"package": "com.real"}}
    linux = {"package": "linux.app", "kind": "linux", "analysis": {"engine": "Linux"}}
    stub = {"package": "com.owlchemylabs.jobsimulator", "added": 1.0, "last_played": 2.0}
    path.write_text(json.dumps({"games": {"com.real": real, "linux.app": linux, stub["package"]: stub},
                                "settings": {}}))
    assert sorted(g["package"] for g in library.games()) == ["com.real", "linux.app"]
    assert "drop_stub_entries" in library.setting("migrations")
