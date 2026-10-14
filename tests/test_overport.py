import subprocess
import zipfile
from pathlib import Path

import pytest

from frameport.apk import axml
from frameport.core.events import Reporter
from frameport.tools import overport

NAME_FAILURE = ("java.util.NoSuchElementException: Sequence is empty.\n"
                "at moe.crx.overport.patching.PatcherContext.appName(PatcherContext.kt:231)")


def manifest(app_label=False):
    from conftest import build_axml

    return build_axml([
        ("start", "manifest", []), ("start", "application", [("label", "str", "Game")] if app_label else []),
        ("start", "activity", [("label", "str", "Game")]), ("end", "activity"),
        ("end", "application"), ("end", "manifest"),
    ])


@pytest.mark.parametrize("retry_fails", [False, True])
def test_overport_retries_missing_app_name_with_a_temporary_apk(monkeypatch, tmp_path, retry_fails):
    apk = tmp_path / "source.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", manifest())
        z.writestr("assets/unchanged", b"game data")
    original = apk.read_bytes()
    calls = []
    outdir = tmp_path / "out"
    monkeypatch.setattr(overport, "_jar", lambda: "overport.jar")

    def run(args):
        source = Path(next(a.removeprefix("--input=") for a in args if a.startswith("--input=")))
        calls.append(source)
        if len(calls) == 1:
            return subprocess.CompletedProcess(args, 1, "", NAME_FAILURE)
        with zipfile.ZipFile(source) as z:
            x = axml.Axml(z.read("AndroidManifest.xml"))
            app = next(el for el in x.elements() if el.name == "application")
            assert x.attr_str(app, "label") == "Game"
            assert z.read("assets/unchanged") == b"game data"
        if retry_fails:
            return subprocess.CompletedProcess(args, 1, "", NAME_FAILURE)
        (outdir / "patched.apk").write_bytes(b"output")
        return subprocess.CompletedProcess(args, 0, "done", "")

    monkeypatch.setattr(overport.toolchain, "run_java", run)
    if retry_fails:
        with pytest.raises(RuntimeError, match="OVRPort failed"):
            overport.patch(apk, outdir, "patched.apk", ["patch_copy_libraries"], Reporter())
    else:
        result = overport.patch(apk, outdir, "patched.apk", ["patch_copy_libraries"], Reporter())
        assert result == outdir / "patched.apk"
    assert len(calls) == 2
    assert calls[0] == apk and calls[1] != apk and not calls[1].exists()
    assert apk.read_bytes() == original


@pytest.mark.parametrize("error,app_label", [("some other failure", False), (NAME_FAILURE, True)])
def test_overport_does_not_retry_unrelated_failures(monkeypatch, tmp_path, error, app_label):
    apk = tmp_path / "source.apk"
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", manifest(app_label))
    calls = []
    monkeypatch.setattr(overport, "_jar", lambda: "overport.jar")

    def run(args):
        calls.append(args)
        return subprocess.CompletedProcess(args, 1, "", error)

    monkeypatch.setattr(overport.toolchain, "run_java", run)
    with pytest.raises(RuntimeError, match="OVRPort failed"):
        overport.patch(apk, tmp_path / "out", "patched.apk", ["patch_copy_libraries"], Reporter())
    assert len(calls) == 1
