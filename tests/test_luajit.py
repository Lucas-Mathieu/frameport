"""Lua engine substitution must preserve native bindings and their self-references through the PLT."""
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile

import pytest
from elftools.elf.elffile import ELFFile

from frameport.analysis import elf, luajit
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame import artifact, luajit_pointers


def _analysis(extra=None):
    return Analysis(package="example.game", version="1", label="Game", abis=["arm64-v8a"], engine="Unity",
                    xr="OpenXR", graphics="GLES", direct_vrapi=False, libs=["libxlua.so"], launcher_activity=None,
                    has_info_category=False, meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0,
                    oculus_os_classes=False, is_overport_output=False, debuggable=True, extra=extra or {})


@pytest.fixture
def libraries(tmp_path):
    cc = shutil.which("cc")
    if not cc or sys.platform != "linux":
        pytest.skip("native loader regression uses Linux cc")
    original = tmp_path / "original.c"
    original.write_text("""
        int lua_gettop(void *state) { (void)state; return -77; }
        void lua_pushlightuserdata(void **state, void *p) { *state = p; }
        void *lua_touserdata(void **state, int index) { (void)index; return *state; }
        int xlua_get_lib_version(void) { return 105; }
        void *xlua_gl(void *state) { return state; }
        int luaopen_perflib(void *state) { return lua_gettop(state); }
        int game_custom_binding(void *state) { return lua_gettop(state) + 1; }
    """)
    runtime = tmp_path / "runtime.c"
    runtime.write_text("""
        int lua_gettop(void *state) { (void)state; return 42; }
        void lua_pushlightuserdata(void **state, void *p) { *state = p; }
        void *lua_touserdata(void **state, int index) { (void)index; return *state; }
    """)
    loader = tmp_path / "libxlua.so"
    core = tmp_path / luajit_pointers.RUNTIME
    for source, dest in [(original, loader), (runtime, core)]:
        subprocess.run([cc, "-shared", "-fPIC", "-O2", str(source), "-Wl,-rpath,$ORIGIN",
                        f"-Wl,-soname,{dest.name}", "-o", str(dest)], check=True, capture_output=True)
    return loader, core


def _call_library(path):
    # A fresh process avoids reusing a dlopen handle after changing the file.
    script = """
import ctypes, json, sys
d = ctypes.CDLL(sys.argv[1])
print(json.dumps([d.xlua_get_lib_version(), d.luaopen_perflib(None), d.game_custom_binding(None), d.lua_gettop(None)]))
"""
    result = subprocess.run([sys.executable, "-c", script, str(path)], check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def test_redirect_definitions_redirects_dlsym_and_internal_calls(libraries):
    loader, core = libraries
    assert _call_library(loader) == [105, -77, -76, -77]
    original = loader.read_bytes()
    names = elf.dyn_symbols(original, True) & elf.dyn_symbols(core.read_bytes(), True)
    redirected, changed = elf.redirect_exports(original, names)
    assert set(changed) == names
    assert names <= elf.dyn_symbols(redirected, False)
    assert "luaopen_perflib" in elf.dyn_symbols(redirected, True)
    loader.write_bytes(elf.add_needed(redirected, core.name))
    assert _call_library(loader) == [105, 42, 43, 42]
    again, changed = elf.redirect_exports(redirected, names)
    assert again == redirected and changed == []


def test_patch_keeps_binding_code_and_refuses_unreviewed_builds(tmp_path, libraries, monkeypatch):
    loader, core = libraries
    original = loader.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    monkeypatch.setattr(luajit, "XLUA_47BIT_SHA256", frozenset({digest}))
    monkeypatch.setattr(luajit_pointers, "artifact", lambda abi, name: core.read_bytes())
    apk = tmp_path / "game.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("lib/arm64-v8a/libxlua.so", original)
    analysis = _analysis({"xlua_47bit": True})
    patch = base.get("frame.luajit_pointers")
    assert patch.detect(analysis).recommended
    assert not patch.applies(_analysis())
    with ApkWorkspace(apk) as ws:
        ctx = base.ApkContext(ws, analysis, {}, Reporter(), {patch.id: {}})
        assert patch.apply(ctx)
        patched = ws.read_lib("libxlua.so")
        with loader.open("rb") as f:
            section = ELFFile(f).get_section_by_name(".text")
            start, size = section["sh_offset"], section["sh_size"]
        assert patched[start:start + size] == original[start:start + size]
        kept = {"xlua_gl", "xlua_get_lib_version", "luaopen_perflib", "game_custom_binding"}
        assert kept <= elf.dyn_symbols(patched, True)
        assert all(ok for _, ok, _ in patch.validate(ctx))
        assert not patch.apply(ctx)
        ws.put(ws.lib(luajit_pointers.RUNTIME), b"old artifact")
        assert patch.apply(ctx)  # repair only a stale engine artifact
        assert ws.read_lib("libxlua.so") == patched
        ws.put(ws.lib("libxlua.so"), original + b"different build")
        with pytest.raises(ValueError, match="not been verified"):
            patch.apply(ctx)


def test_shipped_engine_is_complete_and_preserves_glref_contract():
    data = artifact("arm64-v8a", luajit_pointers.RUNTIME)
    assert elf.soname(data) == luajit_pointers.RUNTIME
    assert {"luaL_newstate", "lua_pushlightuserdata", "lua_touserdata", "lua_topointer",
            "luaopen_ffi", "luaL_register", "lua_close"} <= elf.dyn_symbols(data, True)
    assert "xlua_get_lib_version" not in elf.dyn_symbols(data, True)  # game's bindings stay in the original ELF
    assert b"full-width lightuserdata runtime enabled" in data
    assert all(va % 16384 == off % 16384 for va, off, _ in elf.load_segments(data))


def test_analysis_recommends_only_the_reviewed_engine(tmp_path, quest_manifest, libraries, monkeypatch):
    from frameport.analysis.detect import analyze
    from frameport.recommend.engine import suggest

    loader, _ = libraries
    data = loader.read_bytes()
    monkeypatch.setattr(luajit, "XLUA_47BIT_SHA256", frozenset({hashlib.sha256(data).hexdigest()}))
    apk = tmp_path / "analysis.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("lib/arm64-v8a/libxlua.so", data)
    analysis = analyze(apk)
    assert analysis.extra["xlua_47bit"]
    assert analysis.extra["xlua_sha256"] == hashlib.sha256(data).hexdigest()
    assert "frame.luajit_pointers" in suggest(analysis, use_catalog=False).patches
    monkeypatch.setattr(luajit, "XLUA_47BIT_SHA256", frozenset())
    analysis = analyze(apk)
    assert not analysis.extra["xlua_47bit"]
    assert analysis.extra["xlua_sha256"] == hashlib.sha256(data).hexdigest()  # review an unknown build from diagnostics
    assert "frame.luajit_pointers" not in suggest(analysis, use_catalog=False).patches
