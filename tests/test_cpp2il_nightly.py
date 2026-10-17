"""Cpp2IL's development build for Unity metadata newer than its release reads (Unity 6.5: v107, GitHub #198)."""
import json
import sys
import zipfile
from pathlib import Path

import pytest

from frameport.analysis import il2cpp
from frameport.tools import cpp2il

ERROR = "---> System.FormatException: Unsupported metadata version found! We support 23-106, got 107"
WANTED = {"Assembly-CSharp/X.cs": ["Run"]}


def test_unsupported_metadata_and_runtime_ids(monkeypatch):
    assert cpp2il.unsupported_metadata("Cpp2IL couldn't read this game's code: " + ERROR)
    assert not cpp2il.unsupported_metadata("Cpp2IL couldn't read this game's code: Unity 2017 isn't supported")
    monkeypatch.setattr(cpp2il.platform, "machine", lambda: "aarch64")
    for plat, rid in (("linux", "linux-arm64"), ("darwin", "osx-arm64"), ("win32", "win-x64")):
        monkeypatch.setattr(cpp2il.sys, "platform", plat)
        assert cpp2il.nightly_rid() == rid
    monkeypatch.setattr(cpp2il.platform, "machine", lambda: "x86_64")
    monkeypatch.setattr(cpp2il.sys, "platform", "linux")
    assert cpp2il.nightly_rid() == "linux-x64"


def test_release_path_ignores_the_development_build(monkeypatch, tmp_path):
    monkeypatch.setattr(cpp2il, "tools_dir", lambda: tmp_path)
    monkeypatch.delenv("FRAMEPORT_CPP2IL", raising=False)
    monkeypatch.setattr(cpp2il.sys, "platform", "linux")
    for d in ("cpp2il-2022.1.0-pre-release.21", cpp2il.NIGHTLY_DIR):
        (tmp_path / d).mkdir()
        (tmp_path / d / "Cpp2IL").write_text("")
    assert cpp2il.path() == tmp_path / "cpp2il-2022.1.0-pre-release.21" / "Cpp2IL"


def test_ensure_nightly_unpacks_the_executable(monkeypatch, tmp_path):
    monkeypatch.setattr(cpp2il, "tools_dir", lambda: tmp_path)
    monkeypatch.setattr(cpp2il.sys, "platform", "linux")
    urls = []

    def download(url, dest, progress=None):
        urls.append(url)
        dest.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(dest, "w") as z:
            z.writestr("Cpp2IL", b"\x7fELF fake")
        return dest

    monkeypatch.setattr(cpp2il.cache, "download", download)
    exe = cpp2il.ensure_nightly()
    assert exe == tmp_path / cpp2il.NIGHTLY_DIR / "Cpp2IL" and exe.read_bytes() == b"\x7fELF fake"
    assert urls[0].startswith("https://nightly.link/SamboyCoding/Cpp2IL/") and urls[0].endswith(".zip")
    assert not (tmp_path / cpp2il.NIGHTLY_DIR / "cpp2il.zip").exists()
    assert cpp2il.ensure_nightly() == exe and len(urls) == 1  # downloaded once


@pytest.fixture
def tools(monkeypatch, tmp_path):
    """Release + development build stand-ins: the release can't read metadata v107."""
    monkeypatch.setattr(il2cpp, "cache_dir", lambda: tmp_path)
    monkeypatch.delenv("FRAMEPORT_CPP2IL", raising=False)
    release, nightly = Path("release/Cpp2IL"), Path("nightly/Cpp2IL")
    monkeypatch.setattr(cpp2il, "ensure", lambda progress=None: release)
    monkeypatch.setattr(cpp2il, "ensure_nightly", lambda progress=None: nightly)
    runs = []

    def run(exe, lib, metadata, unity_version, wanted, timeout):
        runs.append(exe)
        if exe == release:
            raise il2cpp._Unreadable(ERROR)
        return {"Assembly-CSharp/X.cs": {"Run": (0x1000, 0x20)}}, []

    monkeypatch.setattr(il2cpp, "_run", run)
    return release, nightly, runs


def test_find_methods_falls_back_to_the_development_build(tools, tmp_path):
    release, nightly, runs = tools
    found = il2cpp.find_methods(b"lib", b"meta", "6000.5.6f1", WANTED)
    assert found == {"Assembly-CSharp/X.cs": {"Run": (0x1000, 0x20)}} and runs == [release, nightly]
    assert il2cpp.find_methods(b"lib", b"meta", "6000.5.6f1", WANTED) == found and len(runs) == 2  # cached


def test_cached_release_error_is_retried_once_with_the_development_build(tools, tmp_path):
    release, nightly, runs = tools
    cache = il2cpp._cache_file(b"lib", b"meta")
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps({"unity_version": "6000.5.6f1", "error": "Cpp2IL couldn't read this game's code: "
                                                                         + ERROR}))
    assert il2cpp.find_methods(b"lib", b"meta", "6000.5.6f1", WANTED)
    assert runs == [nightly]
    cache.write_text(json.dumps({"unity_version": "6000.5.6f1", "error": "x: " + ERROR, "nightly": True}))
    with pytest.raises(RuntimeError):  # the development build couldn't either: not run again
        il2cpp.find_methods(b"lib", b"meta", "6000.5.6f1", WANTED)
    assert runs == [nightly]


def test_any_release_failure_tries_the_development_build_once(tools, monkeypatch):
    """BattleTalent (Unity 2021.3, GitHub #293): the release crashes (IndexOutOfRangeException in GetTypeName), the
    development build reads it. Any failure of the release gets the one development-build try."""
    release, nightly, runs = tools

    def run(exe, *a):
        runs.append(exe)
        raise il2cpp._Unreadable("System.NullReferenceException: Object reference not set")

    monkeypatch.setattr(il2cpp, "_run", run)
    with pytest.raises(RuntimeError, match="couldn't read this game's code: System.NullReferenceException"):
        il2cpp.find_methods(b"lib", b"meta", "2022.3.1f1", WANTED)
    assert runs == [release, nightly]
    cached = json.loads(il2cpp._cache_file(b"lib", b"meta").read_text())
    assert cached["nightly"] is True and "missing" not in cached  # an error, never "no such classes"
    with pytest.raises(RuntimeError):  # both failed: not run again per patch
        il2cpp.find_methods(b"lib", b"meta", "2022.3.1f1", WANTED)
    assert runs == [release, nightly]


def test_explicit_cpp2il_is_never_swapped(tools, monkeypatch):
    release, nightly, runs = tools
    mine = Path("mine/Cpp2IL")

    def run(exe, *a):
        runs.append(exe)
        raise il2cpp._Unreadable("System.IndexOutOfRangeException: Index was outside the bounds of the array.")

    monkeypatch.setattr(il2cpp, "_run", run)
    with pytest.raises(RuntimeError):
        il2cpp.find_methods(b"lib", b"meta", "2021.3.45f2", WANTED, cpp2il=mine)
    assert runs == [mine]
    il2cpp._cache_file(b"lib", b"meta").unlink()
    monkeypatch.setenv("FRAMEPORT_CPP2IL", str(release))  # the user's override (ensure() returns it)
    with pytest.raises(RuntimeError):
        il2cpp.find_methods(b"lib", b"meta", "2021.3.45f2", WANTED)
    assert runs == [mine, release]


def test_poisoned_cache_is_run_again(tools):
    """An older FramePort cached a Cpp2IL crash as "every class missing"; such an entry is run again."""
    release, nightly, runs = tools
    cache = il2cpp._cache_file(b"lib", b"meta")
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps({"unity_version": "2021.3.45f2", "missing": list(WANTED), "classes": {}}))
    assert il2cpp.find_methods(b"lib", b"meta", "2021.3.45f2", WANTED) == {
        "Assembly-CSharp/X.cs": {"Run": (0x1000, 0x20)}}
    assert runs == [release, nightly]
    # a genuine result (some classes found) is kept
    assert il2cpp.find_methods(b"lib", b"meta", "2021.3.45f2", WANTED) and runs == [release, nightly]


def _fake_cpp2il(tmp_path, script: str) -> Path:
    exe = tmp_path / "bin" / "Cpp2IL"
    exe.parent.mkdir(parents=True, exist_ok=True)
    exe.write_text("#!/bin/sh\n" + script)
    exe.chmod(0o755)
    return exe


@pytest.mark.skipif(sys.platform == "win32", reason="shell-script stand-in for Cpp2IL")
def test_cpp2il_crash_after_making_folders_is_unreadable(monkeypatch, tmp_path):
    """The release's crash on BattleTalent: the DiffableCs folder tree exists, no .cs file, exit 134. That's an
    unreadable game (-> the development build), not "none of the classes are in it"."""
    monkeypatch.setattr(il2cpp, "cache_dir", lambda: tmp_path / "cache")
    monkeypatch.delenv("FRAMEPORT_CPP2IL", raising=False)
    crash = _fake_cpp2il(
        tmp_path / "release",
        """out=""; while [ $# -gt 0 ]; do [ "$1" = --output-to ] && out=$2; shift; done
mkdir -p "$out/DiffableCs/Assembly-CSharp"
echo "Unhandled exception. System.IndexOutOfRangeException: Index was outside the bounds of the array." >&2
echo "   at Cpp2IL.Core.Utils.CsFileUtils.GetTypeName(...) in CsFileUtils.cs:line 282" >&2
exit 134
""")
    good = _fake_cpp2il(
        tmp_path / "nightly",
        """out=""; while [ $# -gt 0 ]; do [ "$1" = --output-to ] && out=$2; shift; done
mkdir -p "$out/DiffableCs/Assembly-CSharp"
cat > "$out/DiffableCs/Assembly-CSharp/X.cs" <<'CS'
\t[Address(RVA = "0x5000", Offset = "0x1000", Length = "0x20")]
\tpublic void Run() { }
CS
""")
    monkeypatch.setattr(cpp2il, "ensure", lambda progress=None: crash)
    monkeypatch.setattr(cpp2il, "ensure_nightly", lambda progress=None: good)
    with pytest.raises(il2cpp._Unreadable, match=r"IndexOutOfRangeException.*\(exit code 134\)"):
        il2cpp._run(crash, b"lib", b"meta", "2021.3.45f2", WANTED, 60)
    assert il2cpp.find_methods(b"lib", b"meta", "2021.3.45f2", WANTED) == {
        "Assembly-CSharp/X.cs": {"Run": (0x1000, 0x20)}}
    cached = json.loads(il2cpp._cache_file(b"lib", b"meta").read_text())
    assert cached["nightly"] is True and cached["classes"]
