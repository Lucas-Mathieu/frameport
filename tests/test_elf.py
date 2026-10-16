import struct

import pytest

from frameport.analysis import elf
from frameport.analysis.stubgen import build_stub_library


@pytest.mark.parametrize("abi", ["arm64-v8a", "armeabi-v7a"])
def test_stub_library(abi):
    lib = build_stub_library(["ovr_B", "ovr_A", "ovrMessageType_ToString"], abi=abi)
    assert elf.is_elf(lib)
    assert elf.is_64bit(lib) == (abi == "arm64-v8a")
    assert elf.soname(lib) == "libovrstubs.so"
    assert elf.dyn_symbols(lib, True) == {"ovr_A", "ovr_B", "ovrMessageType_ToString"}
    assert elf.needed(lib) == []
    # 16 KiB page-aligned loads, dynamic inside the RW load
    loads = elf.load_segments(lib)
    assert all(off % 16384 == va % 16384 for va, off, _ in loads)


def _with_note(lib: bytes) -> bytes:
    """Turn the stub's PT_GNU_STACK into a PT_NOTE so add_needed can reuse it (the common case)."""
    buf = bytearray(lib)
    phoff = struct.unpack_from("<Q", buf, 32)[0]
    phentsize, phnum = struct.unpack_from("<H", buf, 54)[0], struct.unpack_from("<H", buf, 56)[0]
    for i in range(phnum):
        if struct.unpack_from("<I", buf, phoff + i * phentsize)[0] == 0x6474E551:
            struct.pack_into("<I", buf, phoff + i * phentsize, 4)
    return bytes(buf)


@pytest.mark.parametrize("variant", ["reuse_note", "relocate_phdrs"])
def test_add_needed(variant):
    base = build_stub_library(["ovr_X"], soname="libgame.so")
    base = elf.add_needed(base, "liblog.so")  # give it one NEEDED first
    if variant == "reuse_note":
        base = _with_note(base)
    out = elf.add_needed(base, "libglshim.so")
    assert elf.needed(out)[0] == "libglshim.so"
    assert elf.needed(out)[1:] == elf.needed(base)
    assert elf.dyn_symbols(out, True) == {"ovr_X"}
    assert elf.soname(out) == "libgame.so"
    assert out[: len(base)][64:] != b"" and out.startswith(base[:16])
    loads = elf.load_segments(out)
    assert [va for va, _, _ in loads] == sorted(va for va, _, _ in loads)
    assert all(off % 16384 == va % 16384 for va, off, _ in loads)
    # idempotent
    assert elf.add_needed(out, "libglshim.so") == out


def _scramble_section_names(lib: bytes) -> bytes:
    """Rotate every section's sh_name by one, like a shipped libOVRPlugin.so whose names are shifted (GitHub #349:
    .dynamic named "dding", .dynsym "text"); types and links stay right."""
    buf = bytearray(lib)
    shoff = struct.unpack_from("<Q", buf, 40)[0]
    shentsize, shnum = struct.unpack_from("<H", buf, 58)[0], struct.unpack_from("<H", buf, 60)[0]
    names = [struct.unpack_from("<I", buf, shoff + i * shentsize)[0] for i in range(1, shnum)]
    for i, name in enumerate(names[1:] + names[:1], start=1):
        struct.pack_into("<I", buf, shoff + i * shentsize, name)
    return bytes(buf)


def test_scrambled_section_names():
    base = elf.add_needed(build_stub_library(["ovr_X", "ovr_Y"], soname="libgame.so"), "liblog.so")
    lib = _scramble_section_names(base)
    assert elf.dynsym_section(elf._elf(lib)) is not None
    assert elf.dyn_symbols(lib, True) == {"ovr_X", "ovr_Y"}
    assert elf.find_symbols(lib, ["ovr_Y"]).keys() == {"ovr_Y"}
    out = elf.add_needed(lib, "libfp_x.so")  # raised "produced NEEDED=[...]" when sections were found by name
    assert elf.needed(out) == ["libfp_x.so", "liblog.so"]
    assert elf.dyn_symbols(out, True) == {"ovr_X", "ovr_Y"}
