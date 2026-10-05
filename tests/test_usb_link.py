"""USB cable link check (frame/usb.py): finding this PC's address on the Frame's USB network."""
from types import SimpleNamespace

from frameport.frame import usb


def test_pc_usb_addresses_from_ipconfig(monkeypatch):
    ipconfig = ("Ethernet adapter Ethernet 3:\n   IPv4 Address. . . . . . . . . . . : 10.86.200.234\n"
                "Wireless LAN adapter Wi-Fi:\n   IPv4 Address. . . . . . . . . . . : 192.168.1.20\n")
    monkeypatch.setattr(usb.subprocess, "run",
                        lambda cmd, **k: SimpleNamespace(stdout=ipconfig if cmd[0] == "ipconfig" else ""))
    assert usb.pc_usb_addresses("10.86.200.233") == ["10.86.200.234"]


def test_check_reports_missing_usb_link():
    frame = SimpleNamespace(agent=lambda cmd, **k: {"present": False})
    out = usb.check(frame)
    assert out["notes"] and "usb0" in out["notes"][0]
