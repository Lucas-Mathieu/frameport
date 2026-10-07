"""Install links, continued (test_deeplink.py has the main cases): cancelled downloads, FrameDrop's placeholder
checksum, which file a manifest installs, naming the program that owns a scheme, lone Windows programs."""
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

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


# ------------------------------------------------------------------------------------------------- titles
@pytest.mark.parametrize("name, title", [
    ("net.sourceforge.opencamera_96.apk", "Opencamera"), ("Cool_Game-v1.2.3-arm64-v8a.apk", "Cool Game"),
    ("com.example.MyGame.apk", "MyGame"), ("SuperTuxKart-1.4-linux-x86_64.tar.xz", "SuperTuxKart"),
    ("thing-linux-aarch64.tar.gz", "Thing")])
def test_titles_from_file_names(name, title):
    assert deeplink.title_from_filename(name) == title


# ------------------------------------------------------------------------------------------------- FramePort fields
def _manifest(extra):
    return deeplink.manifest_from_data({"schema": deeplink.SCHEMA, "name": "G", "files": [{"url": APK}],
                                        "frameport": extra})


def test_frameport_fields():
    m = _manifest({"description": "  A game.  ", "icon": "https://cdn.example.com/icon.png"})
    assert m.description == "A game." and m.icon == "https://cdn.example.com/icon.png"


@pytest.mark.parametrize("extra", [{"icon": "http://cdn.example.com/i.png"}, {"icon": "https://10.0.0.1/i.png"},
                                   {"description": 5, "icon": 7}, "not a dict"])
def test_bad_frameport_fields_are_ignored(extra):
    m = _manifest(extra)
    assert m.icon is None and m.description == ""


def test_long_description_is_cut():
    assert len(_manifest({"description": "x" * 5000}).description) == deeplink.MAX_DESCRIPTION


def _serve(tmp_path):
    www = tmp_path / "www"
    www.mkdir(exist_ok=True)
    httpd = _Server(("127.0.0.1", 0), partial(_Handler, directory=str(www)))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, www, f"http://127.0.0.1:{httpd.server_address[1]}"


def test_icon_is_fetched_and_checked(tmp_path):
    from PIL import Image

    httpd, www, base = _serve(tmp_path)
    try:
        Image.new("RGB", (128, 96), "red").save(www / "icon.jpg")
        Image.new("RGB", (16, 16), "red").save(www / "tiny.png")
        (www / "junk.png").write_bytes(b"not an image")
        m = deeplink.Manifest("G", [deeplink.ManifestFile(APK)], icon=f"{base}/icon.jpg")
        icon = deeplink.fetch_icon(m)
        assert icon and icon.suffix == ".png" and Image.open(icon).size == (128, 96)
        for bad in ("tiny.png", "junk.png", "missing.png"):
            m.icon = f"{base}/{bad}"
            assert deeplink.fetch_icon(m) is None
    finally:
        httpd.shutdown()


def test_description_fills_missing_details_only(monkeypatch, tmp_path):
    path = tmp_path / "Tool.exe"
    path.write_bytes(b"MZ")
    library.upsert_game("rift.tool", kind="rift", details={"sources": []})  # no store description
    monkeypatch.setattr(pipeline, "add_windows_exe", lambda p, r=None: library.game("rift.tool"))
    m = deeplink.Manifest("Tool", [deeplink.ManifestFile("https://c.example.com/Tool.exe")], "https://c.example.com/t.json",
                          description="Does things.")
    g = pipeline.add_from_link(m, path)
    assert g["details"]["description"] == "Does things." and g["title"] == "Tool" and g["title_locked"]
    library.upsert_game("rift.tool", details={"description": "From the store."})
    assert pipeline.add_from_link(m, path)["details"]["description"] == "From the store."


# ------------------------------------------------------------------------------------------------- handler scripts
def test_scripts_wait_for_the_open_window_to_take_the_link():
    """A heartbeat left by a window that was just closed must not swallow the link (seen on Windows)."""
    from pathlib import Path

    ps1 = urlhandler.ps1_script(Path("C:/fp"), ["C:/FramePort/FramePort.exe"])
    assert "Test-Path $queued" in ps1 and ps1.index("Test-Path $queued") < ps1.index("Start-Process")
    sh = urlhandler.sh_script(Path("/fp"), ["/usr/bin/python3", "-m", "frameport.ui.app"])
    assert '[ -e "$queued" ] || exit 0' in sh and sh.index("$queued") < sh.index("setsid")


def test_sh_handler_starts_the_app_when_a_stale_heartbeat_never_takes_the_link(tmp_path):
    import subprocess

    data = tmp_path / "data"
    data.mkdir()
    (data / "gui.alive").touch()  # fresh, but nobody takes links
    marker = tmp_path / "started"
    script = tmp_path / "h.sh"
    script.write_text(urlhandler.sh_script(data, ["touch", str(marker)]))
    subprocess.run(["sh", str(script), "frameport://install?url=x"], timeout=20, check=True)
    for _ in range(40):
        if marker.exists():
            break
        import time

        time.sleep(0.1)
    assert marker.exists() and len(list((data / "links").glob("*.link"))) == 1
