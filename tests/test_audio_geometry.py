"""Batman audio ownership and the scope of the adapter update."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.mark.parametrize("fixture", ["audio_geometry_test.c", "audio_geometry_target_test.c"])
def test_audio_geometry_native(tmp_path, fixture):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("Native ownership regression requires a POSIX C compiler with pthreads")
    root = Path(__file__).resolve().parents[1]
    binary = tmp_path / "audio-geometry-test"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root / "native/adapter"),
                    str(root / "tests/fixtures/src" / fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_audio_geometry_update_outdates_batman_only(monkeypatch):
    from frameport.patches.base import get, recipe_fingerprint

    patch = get("frame.adapter")
    recipe = {"patches": {"frame.adapter": {}}}
    packages = ("com.camouflaj.manta", "com.Armature.VR4", "com.crytek.climb2", "com.example.other")
    current = {p: recipe_fingerprint(recipe, p) for p in packages}
    assert patch.revision_for(packages[0]) == 2
    assert all(patch.revision_for(p) == 1 for p in packages[1:])
    monkeypatch.setattr(patch, "package_revisions", {})
    assert current[packages[0]] != recipe_fingerprint(recipe, packages[0])
    assert all(current[p] == recipe_fingerprint(recipe, p) for p in packages[1:])


def test_audio_geometry_allocation_triage_is_specific_to_batman():
    from frameport.validate.triage import triage

    log = ("package com.camouflaj.manta\n"
           "E Unity : Could not allocate memory: System out of memory!\n"
           "E Unity : Trying to allocate: 26388238103B with 16 alignment. MemoryLabel: NativePlugin\n"
           "E Unity : Allocation happened at: MetaXRAudioUnityMemoryAllocator.cpp\n"
           "F libc : Fatal signal 5 (SIGTRAP)\n")
    result = triage(log, "EXITED")
    finding = next(f for f in result.findings if f.id == "batman-audio-geometry-allocation")
    assert finding.severity == "fatal" and finding.suggest == ["frame.adapter"]
    assert "native-crash" not in {f.id for f in result.findings}
    for ordinary in (log.replace("com.camouflaj.manta", "com.example.other"),
                     log.replace("26388238103B", "4096B"),
                     log.replace("MetaXRAudioUnityMemoryAllocator.cpp", "OtherAllocator.cpp")):
        assert "batman-audio-geometry-allocation" not in {f.id for f in triage(ordinary, "EXITED").findings}
