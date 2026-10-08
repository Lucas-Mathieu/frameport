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
