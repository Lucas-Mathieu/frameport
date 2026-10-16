import struct

import pytest
from test_unreal_splash import context, engine

from frameport.analysis import detect, elf
from frameport.apk.workspace import ApkWorkspace
from frameport.patches.frame import unreal_noreserve as nr

SLOT = 0x123458  # mmap's GOT slot (any address: _mmap_slots is replaced below)


def adrp(rd: int, pc: int, target: int) -> int:
    pages = (target >> 12) - (pc >> 12)
    return 0x90000000 | (pages & 3) << 29 | ((pages >> 2) & 0x7FFFF) << 5 | rd


def bl(pc: int, target: int) -> int:
    return 0x94000000 | ((target - pc) >> 2) & 0x3FFFFFF


def mmap_engine(prot=nr.MOV_W2_RW, flags=nr.MOV_W3_FLAGS):
    """An engine library whose code calls mmap(NULL, size, prot, flags, -1, 0) through a PLT-style stub."""
    data, off = engine(symbol="FAndroidVirtualReserve", size=0x80, prologue=b"")
    va = next(v for v, o, s in elf.load_segments(data) if o <= off < o + s) + off - next(
        o for v, o, s in elf.load_segments(data) if o <= off < o + s)
    stub = va + 0x40
    call = [nr.MOV_X0_NULL, prot, flags, 0x12800004, 0xAA1F03E5]  # mov w4, #-1; mov x5, xzr
    code = call + [bl(va + 4 * len(call), stub)]
    words = code + [0xD503201F] * (16 - len(code))  # nop up to the stub
    words += [adrp(16, stub, SLOT), 0xF9400211 | ((SLOT & 0xFFF) // 8) << 10, 0x91000210, 0xD61F0220]
    buf = bytearray(data)
    struct.pack_into(f"<{len(words)}I", buf, off, *words)
    return bytes(buf), off + 8  # file offset of the flags instruction


@pytest.fixture(autouse=True)
def fake_got(monkeypatch):
    monkeypatch.setattr(nr, "_mmap_slots", lambda data: {SLOT})


def test_reservation_gets_map_noreserve_and_survives_rebuild(tmp_path):
    data, flags_at = mmap_engine()
    assert nr.mmap_sites(data) == ([flags_at], [])
    patch = nr.UnrealNoReserve()
    ctx = context(tmp_path, data)
    with ctx.ws:
        assert patch.apply(ctx)
        fixed = ctx.ws.read_lib("libUE4.so")
        assert fixed == data[:flags_at] + struct.pack("<I", nr.MOV_W3_NORESERVE) + data[flags_at + 4:]
        assert patch.validate(ctx)[0][1]
        assert not patch.apply(ctx)
        ctx.ws.write(tmp_path / "out.apk")
    with ApkWorkspace(tmp_path / "out.apk") as ws:
        ctx.ws = ws
        assert not patch.apply(ctx)
        assert nr.mmap_sites(ws.read_lib("libUE4.so")) == ([], [flags_at])


@pytest.mark.parametrize("kwargs", [{"prot": 0x52800022}, {"flags": 0x52800043}])  # PROT_READ only; MAP_SHARED
def test_other_mmap_calls_are_left_alone(tmp_path, kwargs):
    data, _ = mmap_engine(**kwargs)
    assert nr.mmap_sites(data) == ([], [])
    ctx = context(tmp_path, data)
    with ctx.ws:
        assert not nr.UnrealNoReserve().apply(ctx)


def test_call_to_another_function_is_left_alone(monkeypatch):
    data, _ = mmap_engine()
    monkeypatch.setattr(nr, "_mmap_slots", lambda data: {SLOT + 8})
    assert nr.mmap_sites(data) == ([], [])


def test_binned3_detection():
    assert detect.unreal_binned3(b"x\0./Runtime/Core/Private/HAL/MallocBinned3.cpp\0y")
    assert detect.unreal_binned3("FMallocBinned3 Attempt to free an unrecognized pointer %p".encode("utf-16-le"))
    # other UE5 builds only name Binned3's console variables
    assert not detect.unreal_binned3("MallocBinned3.FlushThreadCacheMaxWaitTime".encode("utf-16-le"))


def test_suggested_only_for_binned3_unreal():
    patch = nr.UnrealNoReserve()

    class A:
        engine, abis, extra = "Unreal", ["arm64-v8a"], {"unreal_binned3": True}
    assert patch.detect(A()).recommended
    A.extra = {}
    assert patch.detect(A()) is None
    A.engine = "Unity"
    assert not patch.applies(A())
