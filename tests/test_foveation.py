"""Shared injection settings and reversible APK diagnostic integration."""
import os
import zipfile
from pathlib import Path
from types import SimpleNamespace

import pytest

from frameport.analysis import elf
from frameport.analysis.stubgen import build_stub_library
from frameport.apk.workspace import ApkWorkspace
from frameport.build import apk_patches
from frameport.core.events import Reporter
from frameport.core.models import Recipe
from frameport.install.installer import install_context
from frameport.patches import base
from frameport.patches.frame import foveation
from frameport.patches.settings import adapter_settings, foveation_options


def context(params, **patches):
    return install_context(Recipe("test.game", patches={"device.foveation": params, **patches}))


def test_default_profile_and_conflicting_native_path():
    ctx = context({"mode": "tracked"}, **{
        "device.lepton_env": {"env": {"FDM_DEBUG": "disable_offsets,debug", "VK_INSTANCE_LAYERS": "",
                                      "LEPTON_ENV_DISABLE_VULKAN_FDM_INJECTION_LAYER": "1"}},
        "adapter.foveation_fix": {"value": 0}})
    assert ctx.env == {"ENABLE_VULKAN_FDM_INJECTION_LAYER": "1", "FDM_DEBUG": "enable"}
    assert ctx.adapter_settings["foveation_fix"] == 1
    assert adapter_settings({"adapter.foveation_fix": {"value": 0}})["foveation_fix"] == 0


def test_injection_keeps_swapchains_ordinary():
    # GitHub #303: a SUBSAMPLED eye image under Valve's FDM showed garbage around the sharp area
    for mode in ("tracked", "fixed"):
        assert adapter_settings({"device.foveation": {"mode": mode}})["foveation_storage_fix"] == 1
    for mode in ("", "off"):
        assert "foveation_storage_fix" not in adapter_settings({"device.foveation": {"mode": mode}})
    src = (Path(__file__).resolve().parents[1] / "native/adapter/frame_adapter.c").read_text()
    assert "if (foveation_storage_fix) {" in src  # not only when foveation_fix is off


def test_custom_size_and_fixed_mode_and_reset():
    assert context({"mode": "tracked", "test": True}).env["LEPTON_ENV_FRAMEPORT_FOVEATION_TEST"] == "1"
    assert "LEPTON_ENV_FRAMEPORT_FOVEATION_TEST" not in context({"mode": "tracked"}).env
    assert context({"mode": "tracked", "radius": 0.35}).env["FOVE_INNER"] == "0.35"
    assert context({"mode": "tracked", "radius": 0.35}).env["FOVE_OUTER"] == "0.7"
    assert context({"mode": "fixed"}).env["FDM_DEBUG"] == "enable,disable_offsets"
    assert context({"mode": "off"}).env == {
        "VK_INSTANCE_LAYERS": "", "LEPTON_ENV_DISABLE_VULKAN_FDM_INJECTION_LAYER": "1"}
    assert not context({"mode": "tracked"}, **{
        "device.lepton_env": {"env": {"FOVE_INNER": "0.1", "FOVE_OUTER": "0.2"}}}).env.keys() & {
            "FOVE_INNER", "FOVE_OUTER"}


@pytest.mark.parametrize("params", [
    {"mode": "bad"}, {"mode": "off", "test": True}, {"mode": "", "radius": 0.2},
    {"mode": "tracked", "radius": float("nan")}, {"mode": "tracked", "radius": float("inf")},
    {"mode": "tracked", "radius": -1}, {"mode": "tracked", "radius": 0.01},
    {"mode": "tracked", "radius": 0.5}, {"mode": "tracked", "test": "false"}])
def test_invalid_configuration_refused(params):
    with pytest.raises(ValueError):
        foveation_options(params)


def test_optional_layer_is_built_and_idempotent(tmp_path, quest_manifest):
    source = tmp_path / "source.apk"
    engine = build_stub_library(["game_main"], soname="libUE4.so")
    with zipfile.ZipFile(source, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("lib/arm64-v8a/libUE4.so", engine)
    patch = base.get("device.foveation")
    assert patch in apk_patches({patch.id: {"mode": "tracked", "test": True}})
    with ApkWorkspace(source) as ws:
        ctx = base.ApkContext(ws, SimpleNamespace(package="test.game"), {"mode": "tracked"}, Reporter(), {})
        assert not patch.apply(ctx)
        assert not ws.has(ws.lib(foveation.LAYER))
        ctx.params["test"] = True
        assert patch.apply(ctx)
        assert foveation.LAYER in elf.needed(ws.read_lib("libUE4.so"))
        assert not patch.apply(ctx)
        assert all(ok for _, ok, _ in patch.validate(ctx))
    # A normal rebuild from the unchanged source drops the optional diagnostic.
    with ApkWorkspace(source) as ws:
        ctx = base.ApkContext(ws, SimpleNamespace(package="test.game"), {"mode": "tracked"}, Reporter(), {})
        assert not patch.apply(ctx)
        assert foveation.LAYER not in elf.needed(ws.read_lib("libUE4.so"))


def test_unrecognized_batman_library_refused():
    with pytest.raises(ValueError, match="unsupported OVRPlugin"):
        foveation.ordinary_storage(build_stub_library(["ovrp_fake"], soname="libOVRPlugin.so"))


@pytest.mark.parametrize("mode", ["tracked", "fixed"])
def test_climb_launcher_isolation(mode):
    patch = {"device.foveation": {"mode": mode}}
    climb = install_context(Recipe("com.crytek.climb2", patches=patch))
    assert climb.env["FDM_DEBUG"].endswith(",yflip")
    assert climb.env["LEPTON_ENV_FRAMEPORT_CLIMB_STEREO_OFFSETS"] == "1"
    for package in ("com.camouflaj.manta", "test.re4", "test.deadpool", "test.asgard"):
        other = install_context(Recipe(package, patches=patch))
        assert not any("CLIMB" in key or "VRAPI" in key for key in other.env)
        assert "yflip" not in other.env["FDM_DEBUG"]
    with pytest.raises(ValueError, match="1.0 resolution"):
        install_context(Recipe("com.crytek.climb2", patches={
            **patch, "adapter.scale": {"value": 1.2}}))


def test_climb_private_bridge_guard_and_rebuild(tmp_path, quest_manifest, monkeypatch):
    import hashlib

    source = tmp_path / "climb.apk"
    renderer = build_stub_library(["vkCreateInstance"], soname="libCryRenderVulkan.so")
    original = build_stub_library(["vrapi_Initialize"], soname="libvrapi.so")
    with zipfile.ZipFile(source, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("lib/arm64-v8a/libCryRenderVulkan.so", renderer)
        z.writestr("lib/arm64-v8a/libvrapi.so", original)
    patch = base.get("device.foveation")
    # The private bridge must survive the regular bridge replacement.
    assert patch.order > base.get("frame.vrapi_bridge").order
    with ApkWorkspace(source) as ws:
        ctx = base.ApkContext(ws, SimpleNamespace(package="com.crytek.climb2"),
                              {"mode": "tracked"}, Reporter(), {})
        with pytest.raises(ValueError, match="unsupported renderer"):
            patch.apply(ctx)
        assert ws.read_lib("libvrapi.so") == original
        monkeypatch.setattr(foveation, "CLIMB_RENDERER", hashlib.sha256(renderer).hexdigest())
        assert patch.apply(ctx)
        assert foveation.LAYER in elf.needed(ws.read_lib("libvrapi.so"))
        assert not patch.apply(ctx)
        assert all(ok for _, ok, _ in patch.validate(ctx))
    # Rebuilding with Off from source removes private APK support.
    with ApkWorkspace(source) as ws:
        ctx = base.ApkContext(ws, SimpleNamespace(package="com.crytek.climb2"),
                              {"mode": "off"}, Reporter(), {})
        assert not patch.apply(ctx)
        assert ws.read_lib("libvrapi.so") == original
        assert not ws.has(ws.lib(foveation.LAYER))


def test_ui_controls_save_size_test_and_mode_without_losing_other_patches(monkeypatch):
    import flet as ft

    from frameport.core import library
    from frameport.ui.views import game

    recipe = Recipe("test.game", patches={"device.foveation": {"mode": "tracked"}, "frame.adapter": {}})
    saved = []
    monkeypatch.setattr(game.engine, "visible_patches", lambda *a: ([base.get("device.foveation")], []))
    monkeypatch.setattr(library, "analysis_from_dict", lambda a: None)
    monkeypatch.setattr(game.pipeline, "set_recipe", lambda package, r: saved.append(library.recipe_to_dict(r)))
    monkeypatch.setattr(ft.Text, "update", lambda self: None)
    view = SimpleNamespace(app=SimpleNamespace(has_game_settings=lambda g: False, open_game=lambda *a, **kw: None),
                           g={"recipe": library.recipe_to_dict(recipe), "analysis": {}},
                           package="test.game", show_all=False)
    panel = game.GameView.advanced_panel(view)

    def walk(control):
        yield control
        for child in getattr(control, "controls", []) or []:
            yield from walk(child)
        if getattr(control, "content", None) is not None:
            yield from walk(control.content)

    controls = list(walk(panel))
    mode = next(c for c in controls if isinstance(c, ft.Dropdown) and c.value == "tracked")
    radius = next(c for c in controls if isinstance(c, ft.Dropdown) and c.value == "0")
    test = next(c for c in controls if isinstance(c, ft.Switch) and "Darken" in (c.label or ""))
    radius.on_select(SimpleNamespace(control=SimpleNamespace(value="0.3")))
    test.on_change(SimpleNamespace(control=SimpleNamespace(value=True)))
    mode.on_select(SimpleNamespace(control=SimpleNamespace(value="fixed")))
    assert saved[-1]["patches"]["device.foveation"] == {"mode": "fixed", "radius": 0.3, "test": True}
    assert "frame.adapter" in saved[-1]["patches"]
    mode.on_select(SimpleNamespace(control=SimpleNamespace(value="off")))
    assert saved[-1]["patches"]["device.foveation"] == {"mode": "off"}


def test_verified_batman_storage_and_other_byte_change_rejected():
    path = os.environ.get("FRAMEPORT_BATMAN_APK")
    if not path:
        pytest.skip("Optional local Batman APK")
    with zipfile.ZipFile(Path(path)) as z:
        original = z.read("lib/arm64-v8a/libOVRPlugin.so")
    fixed = foveation.ordinary_storage(original)
    assert original != fixed and foveation.ordinary_storage(fixed) == fixed
    assert sum(a != b for a, b in zip(original, fixed, strict=True)) == 3
    corrupt = bytearray(fixed)
    corrupt[-1] ^= 1
    with pytest.raises(ValueError, match="verified allocation fix"):
        foveation.ordinary_storage(bytes(corrupt))


def test_verified_climb_apk():
    path = os.environ.get("FRAMEPORT_CLIMB_APK")
    if not path:
        pytest.skip("Optional local Climb APK")
    with ApkWorkspace(Path(path)) as ws:
        ctx = base.ApkContext(ws, SimpleNamespace(package="com.crytek.climb2"),
                              {"mode": "tracked"}, Reporter(), {})
        patch = base.get("device.foveation")
        assert patch.apply(ctx)
        assert all(ok for _, ok, _ in patch.validate(ctx))
        assert not patch.apply(ctx)
