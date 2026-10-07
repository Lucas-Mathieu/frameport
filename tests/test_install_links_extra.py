"""Install links, continued (test_deeplink.py has the main cases): cancelled downloads, FrameDrop's placeholder
checksum, which file a manifest installs, naming the program that owns a scheme, lone Windows programs."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import threading

import pytest

from frameport import deeplink, pipeline, urlhandler
from frameport.core import library
from frameport.core.events import Cancelled, Reporter

APK = "https://cdn.example.com/game-arm64.apk"


class _Server(ThreadingHTTPServer):
    def handle_error(self, request, client_address):  # the cancelled download closes the connection mid-file
        pass


class _Handler(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def test_cancel_removes_the_partial_file(tmp_path):
    www = tmp_path / "www"
    www.mkdir()
    (www / "big.apk").write_bytes(b"x" * (3 << 20))
    httpd = _Server(("127.0.0.1", 0), partial(_Handler, directory=str(www)))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        rep = Reporter()
        rep.cancelled.set()
        dest = tmp_path / "out" / "big.apk"
        with pytest.raises(Cancelled):
            deeplink.download_file(f"http://127.0.0.1:{httpd.server_address[1]}/big.apk", dest, rep)
        assert not dest.exists() and not dest.with_name("big.apk.part").exists()
    finally:
        httpd.shutdown()


def test_placeholder_checksum_is_ignored():
    """FrameDrop's documented example has "sha256": "optional-but-better"."""
    m = deeplink.manifest_from_data({"schema": deeplink.SCHEMA, "name": "G",
                                     "files": [{"url": APK, "sha256": "optional-but-better"}]})
    assert m.main.sha256 is None


def test_main_file_prefers_the_apk():
    m = deeplink.manifest_from_data({"schema": deeplink.SCHEMA, "name": "G", "files": [
        {"url": "https://c.example.com/main.1.g.obb"}, {"url": "https://c.example.com/tool.exe"}, {"url": APK}]})
    assert m.main.url == APK


def test_status_names_the_other_program(monkeypatch):
    handlers = {"framedrop": r'"C:\Program Files\FrameDrop\FrameDrop.exe" "%1"',
                "frameport": f'"powershell.exe" -File "{urlhandler.MARK}.ps1" "%1"'}
    monkeypatch.setattr(urlhandler, "platform_kind", lambda: "windows")
    monkeypatch.setattr(urlhandler, "windows_handler", handlers.get)
    st = urlhandler.status()
    assert st["framedrop"] == "other" and st["framedrop_by"] == "FrameDrop" and st["frameport"] == "ours"


def test_desktop_entry_lists_only_the_enabled_schemes():
    entry = urlhandler.desktop_entry("sh /x.sh %u", ["frameport"])
    assert "MimeType=x-scheme-handler/frameport;\n" in entry and "framedrop" not in entry.split("MimeType")[1]


def test_lone_exe_in_downloads_is_copied_alone(tmp_path, monkeypatch):
    downloads = tmp_path / "Downloads"
    downloads.mkdir()
    (downloads / "huge-unrelated.iso").write_bytes(b"x")
    exe = downloads / "Tool.exe"
    exe.write_bytes(b"MZ")
    seen = {}

    def fake_add(folder, reporter=None, exe=None, force=False, art=False):
        seen.update(folder=folder, exe=exe, force=force)
        return library.upsert_game("rift.tool", kind="rift", title="Tool", exe=exe)
    monkeypatch.setattr(pipeline, "add_rift_game", fake_add)
    g = pipeline.add_windows_exe(exe)
    assert seen["exe"] == "Tool.exe" and seen["force"] and g["exe_confirmed"]
    assert seen["folder"] != downloads and sorted(p.name for p in seen["folder"].iterdir()) == ["Tool.exe"]


def test_exe_in_its_own_folder_keeps_the_folder(tmp_path, monkeypatch):
    game = tmp_path / "Games" / "Tool"
    game.mkdir(parents=True)
    (game / "Tool.exe").write_bytes(b"MZ")
    seen = {}
    monkeypatch.setattr(pipeline, "add_rift_game", lambda folder, *a, **k: seen.update(folder=folder) or
                        library.upsert_game("rift.tool", kind="rift"))
    pipeline.add_windows_exe(game / "Tool.exe")
    assert seen["folder"] == game


def test_not_an_exe_is_refused(tmp_path):
    p = tmp_path / "setup.msi"
    p.write_bytes(b"x")
    with pytest.raises(ValueError, match="isn't a Windows program"):
        pipeline.add_windows_exe(p)
