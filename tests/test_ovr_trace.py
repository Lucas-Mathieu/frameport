"""Meta Platform SDK tracer (native/ovrtrace + patch frame.ovr_trace)."""
from __future__ import annotations

import re
from pathlib import Path

from frameport.analysis import elf
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame import ovr_trace

ROOT = Path(__file__).resolve().parents[1]


def _names():
    return re.findall(r"^N\((ovr_\w+)\)$", (ROOT / "native/ovrtrace/names.inc").read_text(), re.M)


def test_artifact_exports_every_traced_name():
    names = _names()
    assert len(names) == len(set(names)) > 1000
    lib = (ROOT / "artifacts/arm64-v8a" / ovr_trace.LIB).read_bytes()
    exported = elf.dyn_symbols(lib, True)
    assert set(names) <= exported
    assert "ovr_PopMessage" in names and "ovr_Message_GetRequestID" in names


def _a(abis, libs):
    return Analysis(package="x", version="1.0", label="X", abis=abis, engine="Unreal", xr="VrApi", graphics="GLES",
                    direct_vrapi=False, libs=libs, launcher_activity=None, has_info_category=True, meta_permissions=[],
                    uses_glad_gl=False, unity_msaa_levels=0, oculus_os_classes=False, is_overport_output=False,
                    debuggable=False, extra={})


def test_applies_only_to_arm64_games_with_the_platform_loader():
    base.load_all()
    p = base.REGISTRY["frame.ovr_trace"]
    assert p.applies(_a(["arm64-v8a"], ["libUE4.so", "libovrplatformloader.so"]))
    assert not p.applies(_a(["arm64-v8a"], ["libUE4.so"]))
    assert not p.applies(_a(["armeabi-v7a"], ["libovrplatformloader.so"]))
    assert p.detect(_a(["arm64-v8a"], ["libovrplatformloader.so"])) is None
    assert p.experimental and not p.default_on
