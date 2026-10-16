"""action_dedup (FrameBridge, GitHub #323): duplicate controller action names get a free name."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from frameport.patches import base
from frameport.patches.settings import UI, adapter_settings

ROOT = Path(__file__).resolve().parents[1]


def test_duplicate_action_names_get_free_names(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    binary = tmp_path / "action-names-test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/action_names_test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_action_dedup_is_on_by_default():
    base.load_all()
    p = base.REGISTRY["adapter.action_dedup"]
    assert p.default == 1  # the Frame's runtime drops a game with duplicate names: always fix it
    assert adapter_settings({"adapter.action_dedup": {"value": 0}})["action_dedup"] == 0
    assert UI["action_dedup"]["control"] == ("switch",)
    src = (ROOT / "native/adapter/frame_adapter.c").read_text()
    assert 'sscanf(line, "action_dedup=%f"' in src
    assert "if (action_dedup || proximity_emul || profile_remap || input_diag) HOOK_AS(xrCreateAction, hook_xrCreateAction)" in src
