"""fade_quad (FrameBridge, GitHub #197): colour-scale fades drawn as a black quad over the picture."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from frameport.patches import base
from frameport.patches.settings import UI, adapter_settings

ROOT = Path(__file__).resolve().parents[1]


def test_fade_levels_follow_the_colour_scale(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    binary = tmp_path / "fade-level-test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/fade_level_test.c"), "-o", str(binary), "-lm"], check=True)
    subprocess.run([str(binary)], check=True)


def test_fade_quad_is_an_opt_in_game_setting():
    base.load_all()
    p = base.REGISTRY["adapter.fade_quad"]
    assert p.default == 0  # opt-in: the owner asks testers first (Batman's catalog recipe doesn't set it)
    assert adapter_settings({"adapter.fade_quad": {"value": 1}})["fade_quad"] == 1
    assert UI["fade_quad"]["group"] == "picture" and UI["fade_quad"]["control"] == ("switch",)
    # the adapter reads the key (a renamed setting would silently do nothing)
    assert 'sscanf(line, "fade_quad=%f"' in (ROOT / "native/adapter/frame_adapter.c").read_text()
