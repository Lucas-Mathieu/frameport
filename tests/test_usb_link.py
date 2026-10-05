"""USB cable link check (frame/usb.py): finding this PC's address on the Frame's USB network."""
from types import SimpleNamespace

from frameport.frame import usb


def test_pc_usb_addresses_from_ipconfig(monkeypatch):
    ipconfig = ("Ethernet adapter Ethernet 3:\n   IPv4 Address. . . . . . . . . . . : 10.86.200.234\n"
                "Wireless LAN adapter Wi-Fi:\n   IPv4 Address. . . . . . . . . . . : 192.168.1.20\n")
    monkeypatch.setattr(usb.subprocess, "run",
                        lambda cmd, **k: SimpleNamespace(stdout=ipconfig if cmd[0] == "ipconfig" else ""))
    assert usb.pc_usb_addresses("10.86.200.233") == ["10.86.200.234"]


def test_pc_usb_addresses_ignores_broadcast(monkeypatch):
    ip = "7: eth3    inet 10.86.200.234/29 brd 10.86.200.239 scope global eth3\n"
    monkeypatch.setattr(usb.subprocess, "run", lambda cmd, **k: SimpleNamespace(stdout=ip if cmd[0] == "ip" else ""))
    assert usb.pc_usb_addresses("10.86.200.233") == ["10.86.200.234"]


def test_check_reports_missing_usb_link():
    frame = SimpleNamespace(agent=lambda cmd, **k: {"present": False})
    out = usb.check(frame)
    assert out["notes"] and "usb0" in out["notes"][0]


def test_discovered_frames_are_deduplicated_by_host_key():
    from frameport.frame.discovery import Found, dedupe

    found = [Found("frame", "192.168.1.30", source="devkit", addresses=["frame.local"]),
             Found("192.168.1.30", "192.168.1.30", source="scan"),
             Found("My Frame", "10.86.200.233", source="saved"),
             Found("frame", "10.35.78.1", source="devkit"),
             Found("other", "192.168.1.40", source="devkit")]
    keys = {"192.168.1.30": "A", "10.86.200.233": "A", "10.35.78.1": "A", "192.168.1.40": "B"}
    out = dedupe(found, key_of=lambda f: keys.get(f.host))
    assert len(out) == 2
    frame = next(f for f in out if f.host != "192.168.1.40")
    assert frame.host == "10.86.200.233" and frame.via == "USB"  # the fastest link wins
    assert frame.source == "devkit" and frame.name == "frame"  # the best-known source/name is kept
    assert {"192.168.1.30", "10.35.78.1"} <= set(frame.addresses)
    # an entry whose key can't be read isn't merged away
    assert len(dedupe([Found("x", "1.2.3.4")], key_of=lambda f: None)) == 1
