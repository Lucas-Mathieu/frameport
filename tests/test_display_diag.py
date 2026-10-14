"""Desktop diagnostics stay on demand, bounded and useful when Windows queries fail or return no events."""
import json
import subprocess
import sys
from types import SimpleNamespace

import pytest

from frameport.diag import display


@pytest.fixture(autouse=True)
def reset_window(monkeypatch):
    monkeypatch.setattr(display, "_window", {})


def test_non_windows_does_not_start_display_queries(monkeypatch):
    monkeypatch.setattr(display, "_is_windows", lambda: False)

    def unexpected():
        pytest.fail("Windows query ran on another platform")

    monkeypatch.setattr(display, "_windows_displays", unexpected)
    monkeypatch.setattr(display, "_recent_events", unexpected)
    assert display.collect()["windows"] == {"status": "not_applicable"}


def test_window_events_are_cached_without_running_queries(monkeypatch, caplog):
    monkeypatch.setattr(display, "_is_windows", lambda: True)
    monkeypatch.setitem(sys.modules, "flet.version", SimpleNamespace(flet_version="1.0.2", flutter_version="3.44.8"))
    window = SimpleNamespace(width=1280, height=720, left=3086, top=172,
                             minimized=False, maximized=False, full_screen=False)
    with caplog.at_level("INFO", logger="frameport"):
        display.record_window("minimize", window, theme="portal", reduce_motion=True)
    assert "desktop window:" in caplog.text and '"event":"minimize"' in caplog.text
    assert display._window["minimized"] is True and display._window["position"] == [3086, 172]
    monkeypatch.setattr(display, "_windows_displays", lambda: {"status": "ok", "adapters": []})
    monkeypatch.setattr(display, "_recent_events", lambda: {"status": "ok", "events": []})
    result = display.collect()
    assert result["runtime"]["flutter"] == "3.44.8"
    assert result["window"]["event"] == "minimize"
    display.record_window("focus", window, theme="portal", reduce_motion=True)
    display.record_window("restore", window, theme="portal", reduce_motion=True)
    assert display._window["focused"] is True and display._window["minimized"] is False


@pytest.mark.parametrize("response,status", [
    ('{"status":"ok","events":[]}', "ok"),
    ('{"status":"error","error":"AccessDenied","events":[]}', "error"),
    ('{"status":"ok","events":"invalid"}', "error"),
    ("not JSON", "error"),
])
def test_event_query_distinguishes_empty_results_from_failures(monkeypatch, response, status):
    def run(args, **kwargs):
        assert args[:4] == ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive"]
        assert kwargs["timeout"] == 5 and kwargs["creationflags"] == 0x08000000
        assert "-FilterXPath" in args[-1] and "-MaxEvents 60" in args[-1]
        assert ".Message" not in args[-1]  # event metadata, not unrelated free text or device IDs
        return subprocess.CompletedProcess(args, 0, response, "")

    monkeypatch.setattr(display.subprocess, "run", run)
    result = display._recent_events()
    assert result["status"] == status
    if status == "ok":
        assert result["events"] == []


def test_event_query_timeout_does_not_look_like_no_events(monkeypatch):
    def run(*args, **kwargs):
        raise subprocess.TimeoutExpired(args[0], kwargs["timeout"])

    monkeypatch.setattr(display.subprocess, "run", run)
    assert display._recent_events() == {"status": "timeout", "timeout_seconds": 5}


def test_display_failure_does_not_discard_event_or_window_data(monkeypatch):
    monkeypatch.setattr(display, "_is_windows", lambda: True)

    def broken():
        raise OSError("display API unavailable")

    monkeypatch.setattr(display, "_windows_displays", broken)
    monkeypatch.setattr(display, "_recent_events", lambda: {"status": "ok", "events": []})
    result = display.collect()
    assert result["windows"]["displays"]["status"] == "error"
    assert result["windows"]["system_events"]["status"] == "ok"
    json.dumps(result)
