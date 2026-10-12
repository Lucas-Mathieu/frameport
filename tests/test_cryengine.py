"""Monolithic CryEngine APKs and their external content (The Climb, GitHub #238)."""
import zipfile

import pytest
from conftest import build_axml

from frameport.analysis import detect
from frameport.recommend import engine


def cry_apk(path, *, launcher=b"", project=b'{"type": "CRYENGINE Project"}', config=b"r_Driver = vk\n",
            project_path="assets/game.cryproject"):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", build_axml([
            ("start", "manifest", [("package", "str", "com.crytek.climb")]), ("end", "manifest")]))
        z.writestr(project_path, project)
        z.writestr("assets/system.cfg", config)
        z.writestr("lib/arm64-v8a/libAndroidLauncher.so", launcher)
        z.writestr("lib/arm64-v8a/libvrapi.so", b"")
    return path


@pytest.mark.parametrize("deep", [False, True])
@pytest.mark.parametrize("project_path", ["assets/game.cryproject", "assets/climb.cryproject"])
def test_monolithic_cryengine_with_vulkan_config(tmp_path, deep, project_path):
    a = detect.analyze(cry_apk(tmp_path / "game.apk", project_path=project_path), deep=deep)
    assert a.engine == "CryEngine" and a.graphics.startswith("Vulkan")
    r = engine.suggest(a, use_catalog=False)
    assert "frame.vrapi_bridge" in r.patches
    assert r.params("device.files")["files"]["user.cfg"] == "r_variable_rate_shading = 0\n"


def test_cryengine_uses_the_builds_foveation_cvar(tmp_path):
    a = detect.analyze(cry_apk(tmp_path / "game.apk", launcher=b"\0r_vr_foveated\0"))
    r = engine.suggest(a, use_catalog=False)
    assert r.params("device.files")["files"]["user.cfg"] == "r_vr_foveated = 0\n"


@pytest.mark.parametrize("project", [b"not json", b"[]", b'{"type": "another project"}'])
def test_project_filename_alone_is_not_cryengine(tmp_path, project):
    assert detect.analyze(cry_apk(tmp_path / "game.apk", project=project)).engine == "Other"


@pytest.mark.parametrize("config", [b"; r_Driver = vk\nr_Driver = GLES\n", b"-- r_Driver = vk\n", b""])
def test_cryengine_does_not_assume_vulkan(tmp_path, config):
    assert not detect.analyze(cry_apk(tmp_path / "game.apk", config=config)).graphics.startswith("Vulkan")


def test_cryengine_expansion_file_formats_warn_when_data_is_missing(tmp_path):
    from frameport import pipeline
    from frameport.validate.triage import TriageResult, add_missing_obb

    launcher = b"...\0%s/%s.%i.%s.obb\0%s/%s.%s.obb\0..."
    a = detect.analyze(cry_apk(tmp_path / "game.apk", launcher=launcher))
    assert a.extra["expects_obb"]
    entry = {"package": a.package, "analysis": a.to_dict(), "data_dir": None, "data_bytes": 0}
    assert pipeline.missing_obb(entry) and pipeline.analysis_warnings(entry)
    r = add_missing_obb(TriageResult("EXITED", "Lepton container started"))
    assert r.findings[0].id == "missing-obb"
    assert "CryEngine" in r.findings[0].diagnosis
    assert not pipeline.missing_obb({**entry, "data_dir": "obb", "data_bytes": 10})
    assert not detect.analyze(cry_apk(tmp_path / "no-obb.apk")).extra["expects_obb"]


@pytest.mark.parametrize("analysis_version", [11, 12])
def test_monolithic_cryengine_analysis_is_refreshed(tmp_path, analysis_version):
    from frameport import pipeline
    from frameport.core import library

    apk = cry_apk(tmp_path / "game.apk", launcher=b"\0%s/%s.%s.obb\0r_vr_foveated\0")
    a = detect.analyze(apk)
    a.engine = "Other" if analysis_version < 12 else "CryEngine"
    a.graphics = "GLES or unknown (no Vulkan declaration)"
    a.extra.update(analysis_version=analysis_version, expects_obb=False, cryengine_vr_foveated=False)
    old_recipe = library.recipe_to_dict(engine.suggest(a, use_catalog=False))
    library.upsert_game(a.package, apk=str(apk), analysis=a.to_dict(), recipe=old_recipe, suggested=old_recipe)
    assert pipeline.refresh_analyses() == 1
    g = library.game(a.package)
    assert g["analysis"]["engine"] == "CryEngine" and g["analysis"]["extra"]["expects_obb"]
    assert g["recipe"]["patches"]["device.files"]["files"]["user.cfg"] == "r_vr_foveated = 0\n"
