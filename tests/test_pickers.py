"""File-picker fallback without the desktop portal (GitHub #208): tool choice, command lines, results."""
import subprocess
from types import SimpleNamespace

import pytest

from frameport.core import pickers


def which_of(*names):
    return lambda n: f"/usr/bin/{n}" if n in names else None


def test_portal_missing_only_on_linux():
    exc = RuntimeError("org.freedesktop.DBus.Error.NameHasNoOwner: Could not activate remote peer")
    assert pickers.portal_missing(exc, "linux")
    assert not pickers.portal_missing(exc, "win32")
    assert not pickers.portal_missing(ValueError("x"), "linux")


def test_choose_tool_prefers_zenity_except_on_kde():
    assert pickers.choose_tool({}, which_of("zenity", "kdialog")) == ("zenity", "/usr/bin/zenity")
    kde = {"XDG_CURRENT_DESKTOP": "KDE"}
    assert pickers.choose_tool(kde, which_of("zenity", "kdialog")) == ("kdialog", "/usr/bin/kdialog")
    assert pickers.choose_tool(kde, which_of("zenity")) == ("zenity", "/usr/bin/zenity")
    assert pickers.choose_tool({}, which_of()) is None


def test_zenity_command_lines():
    files = pickers.argv("zenity", "zenity", multiple=True, title="Pick", extensions=["apk", ".exe"], start="/h")
    assert files == ["zenity", "--file-selection", "--title", "Pick", "--filename", "/h/", "--multiple",
                     "--separator", "\n", "--file-filter", "*.apk *.exe"]
    folder = pickers.argv("zenity", "zenity", directory=True, start="/h/")
    assert folder == ["zenity", "--file-selection", "--filename", "/h/", "--directory"]


def test_kdialog_command_lines():
    files = pickers.argv("kdialog", "kd", multiple=True, title="Pick", extensions=["json"], start="/h")
    assert files == ["kd", "--title", "Pick", "--getopenfilename", "/h", "*.json", "--multiple",
                     "--separate-output"]
    assert pickers.argv("kdialog", "kd", directory=True, start="/h") == ["kd", "--getexistingdirectory", "/h"]
    with pytest.raises(ValueError):
        pickers.argv("yad", "yad")


def fake_run(code, out="", err=""):
    return lambda cmd, **kw: SimpleNamespace(returncode=code, stdout=out, stderr=err)


def test_run_results():
    assert pickers.run(["zenity"], fake_run(0, "/a b/x.apk\n/c/y.apk\n\n")) == ["/a b/x.apk", "/c/y.apk"]
    assert pickers.run(["zenity"], fake_run(1)) == []  # cancelled
    with pytest.raises(pickers.PickerError, match="no display"):
        pickers.run(["zenity"], fake_run(5, err="no display"))

    def boom(cmd, **kw):
        raise subprocess.TimeoutExpired(cmd, 1)
    with pytest.raises(pickers.PickerError):
        pickers.run(["/usr/bin/kdialog"], boom)
