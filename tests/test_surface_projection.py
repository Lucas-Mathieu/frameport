"""Video adapter freshness and its native stereo projection regression."""
import shutil
import subprocess
from pathlib import Path

import pytest


def test_video_adapter_update_uses_existing_install_state(monkeypatch):
    from frameport.patches.base import get, recipe_fingerprint
    from frameport.ui.components import install_state

    adapter = get("frame.adapter")
    recipe = {"patches": {"frame.adapter": {}}}
    with monkeypatch.context() as old:
        old.setattr(adapter, "revision", 1)
        previous = recipe_fingerprint(recipe)
    game = {"package": "com.camouflaj.manta", "recipe": recipe,
            "build": {"sha256": "installed", "recipe_fp": previous}}
    frame = {"installed": [{"package": game["package"], "sha256": "installed"}]}
    assert install_state(game, frame) == "outdated"
    game["build"]["recipe_fp"] = recipe_fingerprint(recipe, game["package"])
    assert install_state(game, frame) == "installed"


def test_video_revision_only_outdates_batman(monkeypatch):
    from frameport.patches.base import get, recipe_fingerprint
    from frameport.ui.components import install_state

    adapter = get("frame.adapter")
    recipe = {"patches": {"frame.adapter": {}}}
    with monkeypatch.context() as old:
        old.setattr(adapter, "package_revisions", {})
        previous = recipe_fingerprint(recipe)
    for package, expected in (("com.camouflaj.manta", "outdated"), ("com.example.other", "installed")):
        game = {"package": package, "recipe": recipe, "build": {"sha256": "installed", "recipe_fp": previous}}
        frame = {"installed": [{"package": package, "sha256": "installed"}]}
        assert install_state(game, frame) == expected


def test_video_revision_scopes_older_builds_without_fingerprints():
    from frameport.patches.base import get, revised_since_unrecorded
    from frameport.ui.components import install_state

    recipe = {"patches": {"frame.adapter": {}}}
    assert get("frame.adapter").revision == 1  # preserve upstream's shared revision
    for package, expected in (("com.camouflaj.manta", "outdated"), ("com.example.other", "installed")):
        game = {"package": package, "recipe": recipe, "build": {"sha256": "installed"}}
        frame = {"installed": [{"package": package, "sha256": "installed"}]}
        assert revised_since_unrecorded(recipe, package) is (expected == "outdated")
        assert install_state(game, frame) == expected


def test_projection_shader_validates(tmp_path):
    compiler, validator = shutil.which("glslangValidator"), shutil.which("spirv-val")
    if not compiler or not validator:
        pytest.skip("Shader regression requires glslangValidator and spirv-val")
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / "projection.spv"
    subprocess.run([compiler, "-V", "--target-env", "vulkan1.0",
                    str(root / "native/adapter/surface_projection.comp"), "-o", str(binary)], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.0", str(binary)], check=True)
