"""Missing original APK/data files fail before tools or reanalysis run (#278)."""
from unittest.mock import Mock

import pytest

from frameport import pipeline
from frameport.errors import MissingGameSource, explain


@pytest.mark.parametrize("as_is", [False, True])
@pytest.mark.parametrize("directory", [False, True])
def test_build_missing_apk_before_tools(tmp_path, monkeypatch, as_is, directory):
    apk = tmp_path / "base.apk"
    if directory:
        apk.mkdir()
    entry = {"package": "com.example.game", "apk": str(apk), "recipe": {"as_is": as_is}}
    monkeypatch.setattr(pipeline.library, "game", lambda package: entry)
    refresh, build = Mock(), Mock()
    monkeypatch.setattr(pipeline, "refresh_analyses", refresh)
    monkeypatch.setattr(pipeline.builder, "build", build)
    with pytest.raises(MissingGameSource) as caught:
        pipeline.build_game(entry["package"], Mock())
    assert explain(caught.value) == (
        f"The game's APK isn't at {apk} any more. Add the game again from the folder that has it.")
    refresh.assert_not_called()
    build.assert_not_called()


def test_changed_folder_contents_dont_block(tmp_path):
    # a folder scanned without picked files: newer OBBs may replace the ones listed at scan time
    apk = tmp_path / "base.apk"
    apk.write_bytes(b"apk")
    (tmp_path / "obb").mkdir()
    entry = {"package": "com.example.game", "apk": str(apk), "data_dir": str(tmp_path / "obb"),
             "data_manifest": {"main.1.com.example.game.obb": 3}}
    assert pipeline.check_source(entry).apk == apk


@pytest.mark.parametrize("mode", ["folder", "selected"])
def test_missing_saved_data(tmp_path, mode):
    apk = tmp_path / "base.apk"
    apk.write_bytes(b"apk")
    data = tmp_path / "obb"
    entry = {"package": "com.example.game", "apk": str(apk), "data_dir": str(data)}
    if mode != "folder":
        data.mkdir()
        entry["data_files"] = ["main.obb"]
    with pytest.raises(MissingGameSource) as caught:
        pipeline.check_source(entry)
    assert "data" in explain(caught.value) and "Add the game again" in explain(caught.value)
    if mode == "folder":
        data.mkdir()
    else:
        (data / "main.obb").write_bytes(b"obb")
    assert pipeline.source_of(entry).apk == apk


def test_existing_missing_obb_warning_is_nonblocking(tmp_path):
    apk = tmp_path / "base.apk"
    apk.write_bytes(b"apk")
    entry = {"package": "com.example.game", "apk": str(apk), "data_dir": None, "data_bytes": 0,
             "analysis": {"extra": {"expects_obb": True}}}
    assert pipeline.missing_obb(entry)
    assert pipeline.source_of(entry).data_dir is None


@pytest.mark.parametrize("missing", ["apk", "folder", "file"])
def test_source_of_allows_missing_original_files(tmp_path, missing):
    apk = tmp_path / "base.apk"
    data = tmp_path / "obb"
    if missing != "apk":
        apk.write_bytes(b"apk")
    if missing != "folder":
        data.mkdir()
    entry = {"package": "com.example.game", "apk": str(apk), "data_dir": str(data),
             "data_files": ["main.obb"]}
    src = pipeline.source_of(entry)
    assert src.apk == apk and src.data_dir == data


def test_prepare_as_is_checks_source_before_analysis(tmp_path, monkeypatch):
    entry = {"package": "com.example.game", "apk": str(tmp_path / "base.apk")}
    monkeypatch.setattr(pipeline.library, "game", lambda package: entry)
    with pytest.raises(MissingGameSource):
        pipeline.prepare_as_is(entry["package"], Mock())
