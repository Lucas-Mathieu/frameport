"""frame.unity_tls_key_pool (GitHub #198): libunity.so's thread-key imports go to libfp_tkeys.so."""
import struct
import zipfile

import pytest
from elftools.elf.elffile import ELFFile

from frameport.analysis import elf
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame import artifact
from frameport.patches.frame.unity_tls_key_pool import HELPER, RENAMES
from frameport.validate.triage import triage

# the helper itself imports the four pthread functions (with bionic's LIBC version): a real arm64 fixture
LIB = artifact("arm64-v8a", HELPER)


def _imports(data: bytes) -> dict[str, int]:
    """Undefined dynamic symbol -> its .gnu.version index."""
    e = ELFFile(__import__("io").BytesIO(data))
    syms = list(e.get_section_by_name(".dynsym").iter_symbols())
    vs = e.get_section_by_name(".gnu.version")
    raw = data[vs["sh_offset"]:vs["sh_offset"] + vs["sh_size"]]
    return {s.name: struct.unpack_from("<H", raw, i * 2)[0] for i, s in enumerate(syms)
            if s.name and s["st_shndx"] == "SHN_UNDEF"}


def test_rename_imports_in_place():
    before = _imports(LIB)
    assert all(before[n] > 1 for n in RENAMES)  # versioned (LIBC) imports
    out, renamed = elf.rename_imports(LIB, RENAMES)
    assert renamed == sorted(RENAMES)
    assert len(out) == len(LIB)
    after = _imports(out)
    for old, new in RENAMES.items():
        assert old not in after and after[new] == 1  # renamed, version made global
    assert {n: v for n, v in after.items() if n not in RENAMES.values()} == \
        {n: v for n, v in before.items() if n not in RENAMES}  # other imports untouched
    assert elf.dyn_symbols(out, defined=True) == elf.dyn_symbols(LIB, defined=True)
    again, renamed = elf.rename_imports(out, RENAMES)
    assert renamed == [] and again == out


def test_rename_imports_refuses_longer_and_shared_names():
    with pytest.raises(ValueError):
        elf.rename_imports(LIB, {"free": "free_longer"})
    # point another import's name into the tail of pthread_key_create's string ("create"): renaming must refuse
    e = ELFFile(__import__("io").BytesIO(LIB))
    dynsym = e.get_section_by_name(".dynsym")
    syms = list(dynsym.iter_symbols())
    key = next(s for s in syms if s.name == "pthread_key_create")
    other = next(i for i, s in enumerate(syms) if s.name == "calloc")
    buf = bytearray(LIB)
    struct.pack_into("<I", buf, dynsym["sh_offset"] + other * 24, key["st_name"] + len("pthread_key_"))
    with pytest.raises(ValueError):
        elf.rename_imports(bytes(buf), {"pthread_key_create": "fp_tkey_create"})


def _analysis(**kw):
    d = dict(package="com.parkline.wander", version="0.3570", label="Wander", abis=["arm64-v8a"], engine="Unity",
             xr="OpenXR", graphics="GLES", direct_vrapi=False,
             libs=["libunity.so", "libil2cpp.so", "libmain.so", "libFirebaseCppApp-9_6_0.so", "libxul.so"],
             launcher_activity=None, has_info_category=False, meta_permissions=[], uses_glad_gl=False,
             unity_msaa_levels=0, oculus_os_classes=False, is_overport_output=True, debuggable=False,
             extra={"unity_version": "6000.5.6f1"})
    d.update(kw)
    return Analysis(**d)


def test_detect_suggests_for_unity6_with_heavy_libraries():
    p = base.get("frame.unity_tls_key_pool")
    s = p.detect(_analysis())
    assert s and not s.recommended and "libFirebaseCppApp-9_6_0.so" in s.reason
    assert p.detect(_analysis(extra={"unity_version": "2022.3.10f1"})) is None
    assert p.detect(_analysis(libs=["libunity.so", "libil2cpp.so"])) is None
    assert not p.applies(_analysis(engine="Unreal", libs=["libUnreal.so"]))


def test_apply_routes_unitys_keys_through_the_pool(tmp_path, quest_manifest):
    apk = tmp_path / "in.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", b"dex\n035\0")
        z.writestr("lib/arm64-v8a/libunity.so", LIB)
    p = base.get("frame.unity_tls_key_pool")
    with ApkWorkspace(apk) as ws:
        ctx = base.ApkContext(ws, _analysis(), {}, Reporter(), {})
        assert p.apply(ctx)
        assert p.validate(ctx) == [("Thread-key pool present", True, HELPER)]
        first = ws.read_lib("libunity.so")
        p.apply(base.ApkContext(ws, _analysis(), {}, Reporter(), {}))
        assert ws.read_lib("libunity.so") == first  # applying twice changes nothing
        out = ws.write(tmp_path / "out.apk")
    with zipfile.ZipFile(out) as z:
        unity, helper = z.read("lib/arm64-v8a/libunity.so"), z.read(f"lib/arm64-v8a/{HELPER}")
    assert elf.needed(unity)[0] == HELPER
    imports = _imports(unity)
    assert not set(RENAMES) & set(imports) and all(imports[n] == 1 for n in RENAMES.values())
    assert helper == LIB


def test_triage_suggests_the_pool():
    log = ("10-11 18:00:00.000  1200  1230 F libc    : Abort message: 'cannot create thread specific key for "
           "__cxa_get_globals()'\n")
    r = triage(log, "EXITED", "com.parkline.wander")
    f = next(f for f in r.findings if f.id == "pthread-keys-exhausted")
    assert "frame.unity_tls_key_pool" in f.suggest
