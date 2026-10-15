"""Unity heuristics from executable paths and tool support, without catalog/package matching."""
import pytest
from test_patches import _analysis

from frameport.patches import base
from frameport.recommend.engine import suggest

IL2CPP_FIXES = ("frame.unity_text_input", "frame.unity_runtime_msaa_off")


def _il2cpp(**changes):
    values = dict(libs=["libunity.so", "libil2cpp.so", "libOVRPlugin.so"], graphics="GLES",
                  extra={"unity_version": "2019.4.10f1", "text_fields": ["TMP_InputField"],
                         "ovr_runtime_msaa": True})
    values.update(changes)
    return _analysis(**values)


@pytest.mark.parametrize("pid", IL2CPP_FIXES)
@pytest.mark.parametrize("version", ["2017.4.23f1", "2017.1.0f1", "5.6.7f1"])
def test_unreadable_old_metadata_does_not_recommend_il2cpp_rewrites(pid, version):
    a = _il2cpp(extra={"unity_version": version, "text_fields": ["TMP_InputField"], "ovr_runtime_msaa": True})
    p = base.get(pid)
    assert p.detect(a) is None
    assert pid not in suggest(a, use_catalog=False).patches
    assert p.applies(a)  # still available for an explicit recipe; only automatic detection changes


@pytest.mark.parametrize("pid", IL2CPP_FIXES)
@pytest.mark.parametrize("version", ["2018.4.0f1", "2019.4.1f1", "2022.3.1f1", "6000.5.6f1", None, "unknown"])
def test_supported_or_unknown_metadata_keeps_il2cpp_recommendations(pid, version):
    a = _il2cpp(extra={"unity_version": version, "text_fields": ["InputField"], "ovr_runtime_msaa": True})
    assert base.get(pid).detect(a).recommended
    assert pid in suggest(a, use_catalog=False).patches


@pytest.mark.parametrize("pid", IL2CPP_FIXES)
@pytest.mark.parametrize("abis", [["armeabi-v7a"], ["x86_64"], []])
def test_aarch64_return_instructions_are_not_recommended_for_other_abis(pid, abis):
    assert base.get(pid).detect(_il2cpp(abis=abis)) is None


@pytest.mark.parametrize("pid", IL2CPP_FIXES)
def test_il2cpp_rewrites_still_need_unity_and_il2cpp(pid):
    assert base.get(pid).detect(_il2cpp(engine="Unreal")) is None
    assert base.get(pid).detect(_il2cpp(libs=["libunity.so", "libmonobdwgc-2.0.so"])) is None


def test_text_classes_remain_a_signal_for_both_unity_field_implementations():
    p = base.get("frame.unity_text_input")
    for fields in (["TMP_InputField"], ["InputField"], ["TMP_InputField", "InputField"]):
        a = _il2cpp(extra={"text_fields": fields})
        assert p.detect(a).recommended
        assert base.get("device.text_input_window").detect(a).recommended
    assert p.detect(_il2cpp(extra={"text_fields": []})) is None


@pytest.mark.parametrize("graphics", ["Vulkan (declared in manifest)", "Vulkan (Unity boot.config)"])
def test_runtime_msaa_does_not_guess_gles_from_a_vulkan_label(graphics):
    assert base.get("frame.unity_runtime_msaa_off").detect(_il2cpp(graphics=graphics)) is None


def test_runtime_msaa_needs_the_runtime_msaa_signal():
    assert base.get("frame.unity_runtime_msaa_off").detect(_il2cpp(extra={}, unity_msaa_levels=4)) is None


@pytest.mark.parametrize("version", ["2017.4.23f1", "2019.4.20f1", None])
def test_built_in_oculus_path_still_needs_system_app_bypass(version):
    a = _il2cpp(extra={"unity_version": version, "unity_oculus_check": True})
    p = base.get("frame.unity_oculus_check")
    assert p.detect(a).recommended
    assert p.id in suggest(a, use_catalog=False).patches


def test_oculus_xr_plugin_still_gets_the_builtin_startup_check():
    """A verified game (com.coatsink.alone) ships libOculusXRPlugin.so and still needs the bypass."""
    a = _il2cpp(libs=["libunity.so", "libil2cpp.so", "libOVRPlugin.so", "libOculusXRPlugin.so"],
                extra={"unity_version": "2019.4.35f1", "unity_oculus_check": True})
    assert base.get("frame.unity_oculus_check").detect(a).recommended


def test_oculus_metadata_field_alone_does_not_prove_a_native_plugin_path():
    a = _il2cpp(extra={"unity_oculus_check": True, "oculus_xr_plugin": True})
    assert base.get("frame.unity_oculus_check").detect(a).recommended


def test_oculus_bypass_needs_both_the_check_and_ovrplugin():
    p = base.get("frame.unity_oculus_check")
    assert p.detect(_il2cpp(extra={})) is None
    assert p.detect(_il2cpp(libs=["libunity.so"], extra={"unity_oculus_check": True})) is None
    assert p.detect(_il2cpp(engine="Other", extra={"unity_oculus_check": True})) is None


FIRESTORE = "libFirebaseCppFirestore.so"
GECKO = "libxul.so"


def _tls(version="6000.5.6f1", heavy=(FIRESTORE, GECKO), **changes):
    return _analysis(libs=["libunity.so", *heavy], extra={"unity_version": version}, **changes)


@pytest.mark.parametrize("version", ["6000.0.1f1", "6000.5.6f1"])
def test_unity6_firestore_and_gecko_recommend_thread_key_pool(version):
    a = _tls(version)
    p = base.get("frame.unity_tls_key_pool")
    assert p.detect(a).recommended
    assert p.id in suggest(a, use_catalog=False).patches


@pytest.mark.parametrize("heavy", [(FIRESTORE,), (GECKO,), ("libFirebaseCppApp-9_6_0.so", GECKO)])
def test_less_thread_key_pressure_remains_an_optional_diagnostic(heavy):
    a = _tls(heavy=heavy)
    p = base.get("frame.unity_tls_key_pool")
    assert not p.detect(a).recommended
    assert p.id not in suggest(a, use_catalog=False).patches


@pytest.mark.parametrize("version", ["2019.4.1f1", "2022.3.1f1", None, "unknown"])
def test_thread_key_pressure_rule_requires_unity6(version):
    assert base.get("frame.unity_tls_key_pool").detect(_tls(version)) is None


def test_thread_key_pool_requires_native_unity_for_arm64():
    p = base.get("frame.unity_tls_key_pool")
    assert p.detect(_tls(engine="Unreal")) is None
    assert p.detect(_tls(abis=["armeabi-v7a"])) is None
    assert p.detect(_analysis(libs=[FIRESTORE, GECKO], extra={"unity_version": "6000.5.6f1"})) is None


def test_rendering_tradeoffs_stay_opt_in_without_the_runtime_symptom():
    a = _il2cpp(libs=["libunity.so", "libil2cpp.so", "libOculusXRPlugin.so"], unity_msaa_levels=4,
                extra={"oculus_xr_plugin": True, "ovr_runtime_msaa": True})
    for pid in ("frame.unity_multipass", "frame.unity_no_overlay_copy"):
        p = base.get(pid)
        assert p.applies(a) and p.detect(a) is None
        assert pid not in suggest(a, use_catalog=False).patches
