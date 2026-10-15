"""Adapter recommendations use the detected runtime path, without a catalog."""
import pytest
from test_patches import _analysis

from frameport.patches import base
from frameport.recommend.engine import suggest


@pytest.mark.parametrize(("extra", "other_libs", "converted", "expected"), [
    # Built-in Oculus has the system-app check; modern Unity OpenXR can still ship OVRPlugin.
    ({"unity_oculus_check": True}, [], False, True),
    ({"unity_oculus_check": False}, ["libUnityOpenXR.so"], False, False),
    ({"unity_oculus_check": False}, [], False, False),
    # Neither the version nor the graphics API is the reason to select this fix.
    ({"unity_oculus_check": False, "unity_version": "2019.4.20f1"}, [], False, False),
    ({"unity_oculus_check": True, "unity_version": "2019.4.20f1"}, [], False, True),
    # The built-in check can coexist with XR Plugin Management: retain both exclusions.
    ({"unity_oculus_check": True, "oculus_xr_plugin": True}, [], False, False),
    ({"unity_oculus_check": True}, ["libOculusXRPlugin.so"], False, False),
    # Old analyses have no check signal. Converted builds may have the check patched out.
    ({}, [], False, True),
    ({"unity_oculus_check": False}, [], True, True),
    ({"unity_oculus_check": False}, ["libOculusXRPlugin.so"], True, False),
])
def test_pose_time_fix_uses_builtin_oculus_evidence(extra, other_libs, converted, expected):
    a = _analysis(libs=["libunity.so", "libOVRPlugin.so", *other_libs],
                  extra=extra, is_overport_output=converted)
    patch = base.get("adapter.pose_time_fix")
    detected = patch.detect(a)
    assert patch.applies(a)  # still available for manual investigation in any OVRPlugin game
    assert bool(detected and detected.recommended) is expected
    recipe = suggest(a, use_catalog=False)
    assert (patch.id in recipe.patches) is expected
    if expected:
        assert recipe.patches[patch.id] == {"value": 1}
    # The new evidence is specific to pose timing: OVRPlugin's haptic bug affects both paths.
    assert recipe.patches["adapter.haptic_fix"] == {"value": 1}


def test_pose_time_fix_still_requires_ovrplugin():
    a = _analysis(libs=["libunity.so"], extra={"unity_oculus_check": True}, is_overport_output=False)
    patch = base.get("adapter.pose_time_fix")
    assert not patch.applies(a)
    assert patch.detect(a) is None
    assert patch.id not in suggest(a, use_catalog=False).patches


def test_unreal_pose_evidence_is_independent_of_unity():
    a = _analysis(engine="Unreal", libs=["libUE4.so", "libOVRPlugin.so"], is_overport_output=False,
                  extra={"unity_oculus_check": False, "unreal_thumb_touch": True})
    assert suggest(a, use_catalog=False).patches["adapter.pose_time_fix"] == {"value": 1}
