"""Small, read-only desktop diagnostics. Native Windows display queries run only when collecting a bundle."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
from datetime import UTC, datetime

EVENT_TIMEOUT = 5
EVENT_LIMIT = 60
_lock = threading.Lock()
_window: dict = {}


def _now() -> str:
    return datetime.now(UTC).isoformat(timespec="milliseconds")


def _is_windows() -> bool:
    return sys.platform == "win32"


def record_window(event: str, window, *, theme: str, reduce_motion: bool) -> None:
    """Cache and log existing window events; no display queries, timers or extra UI updates."""
    if not _is_windows():
        return
    state = {"event": event, "time_utc": _now(), "size": [window.width, window.height],
             "position": [window.left, window.top], "minimized": window.minimized,
             "maximized": window.maximized, "full_screen": window.full_screen,
             "theme": theme, "reduce_motion": reduce_motion}
    with _lock:
        state["focused"] = (event == "focus") if event in ("focus", "blur") else _window.get("focused")
        if event in ("minimize", "restore"):
            state["minimized"] = event == "minimize"
        if event in ("maximize", "unmaximize"):
            state["maximized"] = event == "maximize"
        if event in ("enter-full-screen", "leave-full-screen"):
            state["full_screen"] = event == "enter-full-screen"
        _window.clear()
        _window.update(state)
    from ..core import applog

    applog.log.info("desktop window: %s", json.dumps(state, separators=(",", ":")))


def _runtime() -> dict:
    # Flet's constants are available in bundles even when wheel metadata is absent. Do not import the GUI here.
    version = sys.modules.get("flet.version")
    return {"flet": getattr(version, "flet_version", None),
            "flutter": getattr(version, "flutter_version", None),
            "executable": os.path.basename(sys.executable),
            "engine_switches": [os.environ[f"FLUTTER_ENGINE_SWITCH_{i}"][:256] for i in range(1, 17)
                                if f"FLUTTER_ENGINE_SWITCH_{i}" in os.environ]}


def _driver(device_key: str) -> dict:
    """Read driver metadata only, without device IDs, registry paths or monitor serials."""
    import winreg

    prefix = "\\Registry\\Machine\\"
    if not device_key.startswith(prefix):
        return {"status": "unavailable"}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, device_key[len(prefix):]) as key:
            out = {}
            for value, field in (("DriverVersion", "version"), ("DriverDate", "date"),
                                 ("ProviderName", "provider")):
                try:
                    out[field] = str(winreg.QueryValueEx(key, value)[0])
                except OSError:
                    pass
            return {"status": "ok" if out else "unavailable", **out}
    except OSError as exc:
        return {"status": "error", "error": str(exc)}


def _windows_displays() -> dict:
    import ctypes as C

    WORD, DWORD, LONG = C.c_uint16, C.c_uint32, C.c_int32

    class Device(C.Structure):
        _fields_ = [("cb", DWORD), ("name", C.c_wchar * 32), ("description", C.c_wchar * 128),
                    ("flags", DWORD), ("device_id", C.c_wchar * 128), ("key", C.c_wchar * 128)]

    class Mode(C.Structure):
        _fields_ = [("device", C.c_wchar * 32), ("spec", WORD), ("driver", WORD), ("size", WORD),
                    ("extra", WORD), ("fields", DWORD), ("x", LONG), ("y", LONG),
                    ("orientation", DWORD), ("fixed_output", DWORD), ("color", WORD), ("duplex", WORD),
                    ("y_resolution", WORD), ("tt_option", WORD), ("collate", WORD),
                    ("form", C.c_wchar * 32), ("log_pixels", WORD), ("bits", DWORD), ("width", DWORD),
                    ("height", DWORD), ("flags", DWORD), ("frequency", DWORD), ("icm_method", DWORD),
                    ("icm_intent", DWORD), ("media_type", DWORD), ("dither", DWORD),
                    ("reserved1", DWORD), ("reserved2", DWORD), ("panning_width", DWORD),
                    ("panning_height", DWORD)]

    user = C.WinDLL("user32", use_last_error=True)
    enum = user.EnumDisplayDevicesW
    enum.argtypes, enum.restype = [C.c_wchar_p, DWORD, C.POINTER(Device), DWORD], C.c_int
    settings = user.EnumDisplaySettingsW
    settings.argtypes, settings.restype = [C.c_wchar_p, DWORD, C.POINTER(Mode)], C.c_int
    adapters = []
    for i in range(16):
        device = Device(cb=C.sizeof(Device))
        if not enum(None, i, C.byref(device), 0):
            break
        adapter = {"name": device.name, "description": device.description,
                   "attached": bool(device.flags & 1), "primary": bool(device.flags & 4),
                   "driver": _driver(device.key)}
        mode = Mode(size=C.sizeof(Mode))
        if settings(device.name, 0xFFFFFFFF, C.byref(mode)):  # ENUM_CURRENT_SETTINGS
            adapter["mode"] = {"size": [mode.width, mode.height], "position": [mode.x, mode.y],
                               "refresh_hz": mode.frequency, "bits_per_pixel": mode.bits,
                               "orientation": mode.orientation}
        else:
            adapter["mode"] = {"status": "unavailable"}
        monitors = []
        for j in range(16):
            monitor = Device(cb=C.sizeof(Device))
            if not enum(device.name, j, C.byref(monitor), 0):
                break
            monitors.append({"name": monitor.name, "description": monitor.description,
                             "active": bool(monitor.flags & 1)})
        adapter["monitors"] = monitors
        adapters.append(adapter)
    return {"status": "ok", "adapters": adapters, "limit_reached": len(adapters) == 16}


# Filter on the event-log service, not by scanning every System event. Metadata is enough to correlate resets
# and device changes; event messages can contain unrelated device IDs and personal data.
_EVENT_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$OutputEncoding = [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$providers = "Provider[@Name='Display'] or Provider[@Name='nvlddmkm'] or " +
             "Provider[@Name='Microsoft-Windows-Kernel-PnP'] or Provider[@Name='Microsoft-Windows-Dwm-Core'] or " +
             "Provider[@Name='Microsoft-Windows-DxgKrnl']"
$xpath = "*[System[TimeCreated[timediff(@SystemTime) <= 1800000] and ($providers)]]"
try {
    $events = @(Get-WinEvent -LogName System -FilterXPath $xpath -MaxEvents 60 -ErrorAction Stop | ForEach-Object {
        @{time_utc=$_.TimeCreated.ToUniversalTime().ToString('o'); provider=$_.ProviderName;
          id=$_.Id; level=$_.Level; record_id=$_.RecordId}
    })
    @{status='ok'; events=$events} | ConvertTo-Json -Depth 4 -Compress
} catch {
    if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') {
        @{status='ok'; events=@()} | ConvertTo-Json -Compress
    } else {
        @{status='error'; error=$_.FullyQualifiedErrorId; events=@()} | ConvertTo-Json -Compress
    }
}
"""


def _recent_events() -> dict:
    from ..updates import _powershell

    try:
        # Windows PowerShell 5.1 without pwsh's PSModulePath: started from pwsh 7 it can't load Get-WinEvent's module
        exe, env = _powershell()
        result = subprocess.run([exe, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
                                 _EVENT_SCRIPT], capture_output=True, encoding="utf-8", errors="replace",
                                timeout=EVENT_TIMEOUT, creationflags=0x08000000, env=env)
        if result.returncode:
            return {"status": "error", "error": (result.stderr or "PowerShell query failed")[-512:]}
        data = json.loads(result.stdout.lstrip("\ufeff"))
        if not isinstance(data, dict) or data.get("status") not in ("ok", "error"):
            raise ValueError("unexpected event query result")
        if not isinstance(data.get("events"), list):
            raise ValueError("unexpected event list")
        data["events"] = data["events"][:EVENT_LIMIT]
        return data
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "timeout_seconds": EVENT_TIMEOUT}
    except (OSError, ValueError, TypeError) as exc:
        return {"status": "error", "error": str(exc)[:512]}


def collect() -> dict:
    """One on-demand snapshot. Unsupported platforms and query failures are explicit, never empty successes."""
    with _lock:
        window = dict(_window)
    out = {"collected_at_utc": _now(), "platform": sys.platform, "runtime": _runtime(), "window": window}
    if not _is_windows():
        out["windows"] = {"status": "not_applicable"}
        return out
    try:
        displays = _windows_displays()
    except Exception as exc:  # noqa: BLE001  diagnostics must not prevent collecting the rest of the bundle
        displays = {"status": "error", "error": str(exc)[:512]}
    out["windows"] = {"displays": displays, "system_events": {"lookback_minutes": 30, **_recent_events()}}
    return out
