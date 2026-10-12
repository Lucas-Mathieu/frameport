import io
import struct

import pytest
from elftools.elf.elffile import ELFFile
from test_unreal_splash import context, engine

from frameport.apk.workspace import ApkWorkspace
from frameport.patches.frame import unreal_multiview_depth as depth


def depth_engine(**kwargs):
    return engine(**dict(symbol=depth.ALLOC, size=depth.SIZE,
                         prologue=bytes(depth.SEQUENCE_OFFSET) + depth.SEQUENCE) | kwargs)


def test_refresh_only_changes_the_cached_size_branch_and_survives_rebuild(tmp_path):
    data, off = depth_engine()
    branch = off + depth.SEQUENCE_OFFSET + 4
    patch = depth.UnrealMultiviewDepth()
    ctx = context(tmp_path, data)
    with ctx.ws:
        assert depth.depth_site(data) == (branch, False)
        assert patch.apply(ctx)
        assert ctx.ws.read_lib("libUE4.so") == data[:branch] + depth.NOP + data[branch + 4:]
        assert patch.validate(ctx)[0][1]
        assert not patch.apply(ctx)
        ctx.ws.write(tmp_path / "out.apk")
    with ApkWorkspace(tmp_path / "out.apk") as ws:
        ctx.ws = ws
        assert not patch.apply(ctx)
        assert patch.validate(ctx)[0][1]


@pytest.mark.parametrize("kwargs", [
    {"symbol": "DifferentFunction"}, {"size": depth.SIZE - 4},
    {"prologue": bytes(depth.SIZE)},
    {"prologue": bytes(depth.SEQUENCE_OFFSET) + depth.SEQUENCE[:8] + bytes(24)},
])
def test_unknown_allocator_is_left_unchanged(tmp_path, kwargs):
    data, _ = depth_engine(**kwargs)
    ctx = context(tmp_path, data)
    with ctx.ws:
        patch = depth.UnrealMultiviewDepth()
        assert not patch.apply(ctx)
        assert not ctx.ws.changed()
        assert not patch.validate(ctx)[0][1]


def test_unmapped_function_and_wrong_abi_are_refused(tmp_path):
    data, _ = depth_engine()
    invalid = bytearray(data)
    e = ELFFile(io.BytesIO(data))
    struct.pack_into("<Q", invalid, e.get_section_by_name(".dynsym")["sh_offset"] + 24 + 8, len(data) + 0x1000)
    assert depth.depth_site(bytes(invalid)) is None
    ctx = context(tmp_path, data, abi="armeabi-v7a")
    with ctx.ws:
        assert not depth.UnrealMultiviewDepth().apply(ctx)
        assert not ctx.ws.changed()


def test_depth_triage_requires_measured_mismatched_attachments():
    from frameport.validate.triage import triage

    color = "ViewLog: attachment 8ce0 type=1702 name=9 renderbuffer=1728x1728\n"
    shallow = "ViewLog: attachment 8d00 type=1702 name=10 renderbuffer=1920x1016\n"
    for trace in (color + shallow, shallow + color, color + shallow.replace("1016", "1020")):
        assert depth.UnrealMultiviewDepth.id in triage(trace, "RUNNING", "com.ilmxlab.tales").suggestions()
    for trace in (color, shallow, color + shallow.replace("1920x1016", "1728x1728")):
        assert depth.UnrealMultiviewDepth.id not in triage(trace, "RUNNING", "com.ilmxlab.tales").suggestions()
