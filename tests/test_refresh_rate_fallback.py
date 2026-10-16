"""FrameBridge answers xrGetDisplayRefreshRateFB with a real rate (GitHub #306, PreyVR's black main menu).

The Frame's runtime has no XR_FB_display_refresh_rate; OVRPort's dispatcher still offers it and answered 0 Hz. Team
Beef's idTech 4 ports (PreyVR, the Doom3Quest engine) run 1000 / refresh ms per tic, so 0 Hz stopped their game and
GUI clock: the main menu stayed at its first animation step (black, only the cursor)."""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

from frameport.validate import triage

ROOT = Path(__file__).resolve().parents[1]


def test_fallback_rate_is_never_zero(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("needs a POSIX host C compiler")
    binary = tmp_path / "refresh-rate-test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "native/adapter"),
                    str(ROOT / "tests/fixtures/src/refresh_rate_test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_the_hook_is_always_installed():
    src = (ROOT / "native/adapter/frame_adapter.c").read_text()
    # not behind a per-game setting: every game that asks gets a rate, also where the runtime lacks the function
    assert re.search(r"\n    if \(instance != XR_NULL_HANDLE\) HOOK_AS\(xrGetDisplayRefreshRateFB, hook_get_refresh_rate\)",
                     src)
    assert "fb_refresh_rate(" in (ROOT / "native/adapter/session_fixes.c").read_text()


def test_a_single_rejected_frame_is_not_an_error():
    entries = {e["id"]: e for e in yaml.safe_load((ROOT / "catalog/triage.yaml").read_text())["signatures"]}
    assert entries["cylinder-menu-layers-rejected"]["severity"] == "info"
    log = "10-16 05:06:16.011  1188  1248 I FrameBridge: xrEndFrame failed -16: blend=1 layers=7\n"
    found = [f for f in triage.triage(log).findings if f.id == "cylinder-menu-layers-rejected"]
    assert found and found[0].severity == "info"
