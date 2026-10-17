"""Install-link updates: a manifest's `version`, remembered with the game and checked again at start."""
import json

import pytest

from frameport import deeplink, pipeline
from frameport.core import library

APK = "https://cdn.example.com/game-arm64.apk"
SRC = "https://cdn.example.com/game.json"


def _data(**kw):
    return {"schema": deeplink.SCHEMA, "name": "G", "files": [{"url": APK}], **kw}


@pytest.mark.parametrize("data,version", [
    (_data(version="1.4.2"), "1.4.2"),
    (_data(version=3), "3"),
    (_data(frameport={"version": " 2.0 "}), "2.0"),
    (_data(version="1.0", frameport={"version": "2.0"}), "2.0"),
    (_data(version=True), ""), (_data(version={"x": 1}), ""), (_data(version="x" * 100), ""), (_data(), ""),
])
def test_manifest_version(data, version):
    assert deeplink.manifest_from_data(data, SRC).version == version


@pytest.mark.parametrize("a,b", [
    ("1.10", "1.9"), ("v2", "1.99.99"), ("1.2.0", "1.2.0-beta3"), ("1.2.0-rc10", "1.2.0-rc2"), ("1.2.1", "1.2"),
    ("1.0.1", "1.0.0b"),
])
def test_newer_versions(a, b):
    assert deeplink.compare_versions(a, b) == 1 and deeplink.compare_versions(b, a) == -1


@pytest.mark.parametrize("a,b", [("1.2", "1.2.0"), ("v1.2", "1.2"), ("build 7", "build 7")])
def test_equal_versions(a, b):
    assert deeplink.compare_versions(a, b) == 0


def test_mixed_versions_dont_crash():
    assert deeplink.compare_versions("1.0-2", "1.0-b") in (-1, 1)
    assert deeplink.compare_versions("nightly", "1.0") in (-1, 1)
    assert not deeplink.is_newer_version("2.0", "") and not deeplink.is_newer_version("", "1.0")


@pytest.mark.parametrize("link,ok", [
    ({"source": SRC, "version": "1.0"}, True),
    ({"source": SRC, "version": ""}, False),
    ({"source": SRC, "version": "1.0", "direct": True}, False),
    ({"source": APK, "version": "1.0"}, False),  # a bare file link
    ({"source": "/home/u/game.json", "version": "1.0"}, False),
    ({"source": deeplink.DEMO_MANIFEST, "version": "1.0"}, False),
    ({"source": "https://10.0.0.2/game.json", "version": "1.0"}, False),
    (None, False),
])
def test_which_links_are_checked(link, ok):
    assert deeplink.can_check_updates(link) is ok


def _add(monkeypatch, tmp_path, version, name="G"):
    path = tmp_path / "Tool.exe"
    path.write_bytes(b"MZ")
    if not library.game("rift.tool"):
        library.upsert_game("rift.tool", kind="rift", details={"sources": []})
    monkeypatch.setattr(pipeline, "add_windows_exe", lambda p, r=None: library.game("rift.tool"))
    m = deeplink.manifest_from_data(_data(name=name, version=version, files=[{"url": "https://c.example.com/T.exe"}]),
                                    SRC)
    return pipeline.add_from_link(m, path)


def test_check_marks_and_clears_updates(monkeypatch, tmp_path):
    g = _add(monkeypatch, tmp_path, "1.0")
    assert g["link"]["version"] == "1.0" and g["link"]["direct"] is False
    monkeypatch.setattr(deeplink, "fetch_manifest", lambda req: deeplink.manifest_from_data(
        _data(version="1.1"), req.manifest_url))
    assert pipeline.check_link_updates(force=True) == ["rift.tool"]
    assert library.game("rift.tool")["link_update"]["version"] == "1.1"
    # within LINK_CHECK_HOURS nothing is fetched, the known offer stays
    monkeypatch.setattr(deeplink, "fetch_manifest", lambda req: pytest.fail("fetched too early"))
    assert pipeline.check_link_updates() == ["rift.tool"]
    # installing the update records its version and clears the offer
    g = _add(monkeypatch, tmp_path, "1.1")
    assert g["link"]["version"] == "1.1" and "link_update" not in g
    # an offer the manifest withdrew goes away
    library.upsert_game("rift.tool", link_update={"version": "9"})
    monkeypatch.setattr(deeplink, "fetch_manifest", lambda req: deeplink.manifest_from_data(_data(version="1.1"), SRC))
    assert pipeline.check_link_updates(force=True) == []

    # a manifest that can't be read keeps the state
    def offline(req):
        raise OSError("offline")
    library.upsert_game("rift.tool", link_update={"version": "1.2"})
    monkeypatch.setattr(deeplink, "fetch_manifest", offline)
    assert pipeline.check_link_updates(force=True) == ["rift.tool"]


def test_check_can_be_switched_off(monkeypatch, tmp_path):
    _add(monkeypatch, tmp_path, "1.0")
    library.set_setting("links.check_updates", False)
    monkeypatch.setattr(deeplink, "fetch_manifest", lambda req: pytest.fail("checked while off"))
    assert pipeline.check_link_updates() == []


def test_update_keeps_the_users_own_name(monkeypatch, tmp_path):
    _add(monkeypatch, tmp_path, "1.0", name="Tool")
    library.upsert_game("rift.tool", title="My Tool", title_locked=True)
    assert _add(monkeypatch, tmp_path, "1.1", name="Tool")["title"] == "My Tool"
    library.upsert_game("rift.tool", title="Tool")  # not renamed: a new name from the manifest is taken
    assert _add(monkeypatch, tmp_path, "1.2", name="Tool 2")["title"] == "Tool 2"


def test_local_server_round_trip(tmp_path):
    """check_update fetches the remembered manifest over HTTP (loopback is allowed for testing)."""
    import threading
    from functools import partial
    from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

    class Quiet(SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass
    (tmp_path / "game.json").write_text(json.dumps(_data(version="2.0")))
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), partial(Quiet, directory=str(tmp_path)))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        link = {"source": f"http://127.0.0.1:{httpd.server_address[1]}/game.json", "version": "1.0"}
        assert deeplink.can_check_updates(link)
        assert deeplink.check_update(link).version == "2.0"
        assert deeplink.check_update({**link, "version": "2.0"}) is None
    finally:
        httpd.shutdown()
