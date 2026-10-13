import io
import struct
import zipfile
from types import SimpleNamespace

import pytest
from elftools.elf.elffile import ELFFile

from frameport.analysis import elf
from frameport.analysis.stubgen import build_stub_library
from frameport.apk.workspace import ApkWorkspace
from frameport.patches.frame import unreal_splash as splash


def engine(symbol=splash.RENDER, size=splash.SIZE, prologue=splash.PROLOGUE):
    data = bytearray(build_stub_library([symbol], soname="libUE4.so"))
    e = ELFFile(io.BytesIO(data))
    va, _ = elf.find_symbols(bytes(data), [symbol])[symbol]
    off = elf.vaddr_to_offset(elf.load_segments(bytes(data)), va)
    data[off:off + size] = (prologue + bytes(size))[:size]
    # The stub has an unused gap before its second LOAD, large enough for the splash function.
    struct.pack_into("<Q", data, e.get_section_by_name(".dynsym")["sh_offset"] + 24 + 16, size)
    struct.pack_into("<QQ", data, 64 + 32, off + size, off + size)
    struct.pack_into("<Q", data, e["e_shoff"] + 4 * 64 + 32, size)
    return bytes(data), off


def context(tmp_path, data, abi="arm64-v8a"):
    apk = tmp_path / "in.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr(f"lib/{abi}/libUE4.so", data)
    return SimpleNamespace(ws=ApkWorkspace(apk), notes=[])


def test_bypass_only_changes_the_entry_instruction_and_survives_rebuild(tmp_path):
    data, off = engine()
    patch = splash.UnrealSplashBypass()
    ctx = context(tmp_path, data)
    with ctx.ws:
        assert splash.splash_site(data) == (off, False)
        assert patch.apply(ctx)
        fixed = ctx.ws.read_lib("libUE4.so")
        assert fixed == data[:off] + splash.RET + data[off + 4:]
        assert patch.validate(ctx)[0][1]
        assert not patch.apply(ctx)
        ctx.ws.write(tmp_path / "out.apk")
    with ApkWorkspace(tmp_path / "out.apk") as ws:
        ctx.ws = ws
        assert not patch.apply(ctx)
        assert patch.validate(ctx)[0][1]


@pytest.mark.parametrize("kwargs", [
    {"symbol": "DifferentFunction"}, {"size": splash.SIZE - 4},
    {"prologue": b"\0" * 32}, {"prologue": splash.PROLOGUE[:8] + b"\0" * 24},
])
def test_unknown_engine_code_is_left_unchanged(tmp_path, kwargs):
    data, _ = engine(**kwargs)
    ctx = context(tmp_path, data)
    with ctx.ws:
        patch = splash.UnrealSplashBypass()
        assert not patch.apply(ctx)
        assert not ctx.ws.changed()
        assert not patch.validate(ctx)[0][1]


def test_unmapped_function_and_wrong_abi_are_refused(tmp_path):
    data, _ = engine()
    invalid = bytearray(data)
    e = ELFFile(io.BytesIO(data))
    struct.pack_into("<Q", invalid, e.get_section_by_name(".dynsym")["sh_offset"] + 24 + 8, len(data) + 0x1000)
    assert splash.splash_site(bytes(invalid)) is None
    ctx = context(tmp_path, data, abi="armeabi-v7a")
    with ctx.ws:
        assert not splash.UnrealSplashBypass().apply(ctx)
        assert not ctx.ws.changed()


def test_stall_triage_requires_both_blocked_threads():
    from frameport.validate.triage import triage

    render = "CSxrSession::WaitFrame\nOculusHMD::FSplash::RenderFrame_RenderThread\n"
    main = "FlushRenderingCommands\nUEngine::LoadMap\n"
    for trace in (render + main, main + render):
        result = triage(trace, "RUNNING", "com.ilmxlab.tales")
        assert "frame.unreal_splash_bypass" in result.suggestions()
    for trace in (render, main, "FSplash::RenderFrame_RenderThread"):
        assert "frame.unreal_splash_bypass" not in triage(trace, "RUNNING", "com.ilmxlab.tales").suggestions()
