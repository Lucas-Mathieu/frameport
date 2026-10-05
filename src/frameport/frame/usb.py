"""USB cable link to the Frame (usb0, a USB network gadget): one check that answers whether "connect with a USB
cable" can work on this PC: what the Frame presents, whether this PC got an address on it, whether SSH answers
over it (same host key), and how fast an upload is compared with the normal connection."""
from __future__ import annotations

import io
import os
import re
import socket
import subprocess
import sys
import time

USB_IFACE = "usb0"
TEST_BYTES = 128 << 20


def pc_usb_addresses(frame_ip: str) -> list[str]:
    """This PC's IPv4 addresses in the same /24 as the Frame's USB address (Windows via ipconfig, else `ip`)."""
    prefix = frame_ip.rsplit(".", 1)[0] + "."
    texts = []
    for cmd in (["ip", "-4", "-o", "addr", "show"], ["ipconfig"], ["ifconfig"], ["ipconfig.exe"]):
        try:
            texts.append(subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout)
        except (OSError, subprocess.SubprocessError):
            continue
    found = re.findall(r"(\d+\.\d+\.\d+\.\d+)", "\n".join(texts))
    return sorted({ip for ip in found if ip.startswith(prefix) and ip != frame_ip})


def upload_speed(frame, size: int = TEST_BYTES) -> float:
    """MB/s of one SFTP upload of `size` bytes to /tmp on the Frame (removed afterwards)."""
    path = f"/tmp/frameport-speed-{os.getpid()}"
    data = io.BytesIO(os.urandom(1 << 20) * (size >> 20))
    start = time.time()
    frame.sftp.putfo(data, path)
    elapsed = time.time() - start
    frame.run(f"rm -f {path}")
    return size / elapsed / 1e6


def check(frame, measure: bool = True) -> dict:
    """{frame: <agent usb_link>, frame_ip, pc_addresses, reachable, same_frame, speed_usb, speed_default, notes}."""
    info = frame.agent("usb_link", timeout=60)
    out = {"frame": info, "platform": sys.platform, "notes": []}
    ip = next((a.split("/")[0] for a in info.get("addresses") or []), None)
    out["frame_ip"] = ip
    if not info.get("present"):
        out["notes"].append("The Frame has no usb0 link: is the cable in the Frame's USB-C port and the PC?")
        return out
    if not ip:
        out["notes"].append("usb0 exists but has no address on the Frame (cable not connected to a PC?)")
        return out
    out["pc_addresses"] = pc_usb_addresses(ip)
    if not out["pc_addresses"]:
        out["notes"].append("This PC has no address on the Frame's USB network: no driver for the gadget "
                            f"({', '.join(info.get('functions') or []) or 'unknown type'}) or no DHCP from the Frame")
    try:
        socket.create_connection((ip, frame.target.port), timeout=3).close()
        out["reachable"] = True
    except OSError:
        out["reachable"] = False
        out["notes"].append(f"SSH ({ip}:{frame.target.port}) doesn't answer over the cable from this PC")
        return out
    from .connection import Frame, FrameTarget

    other = Frame(FrameTarget(ip, frame.target.user, frame.target.port), frame.password)
    try:
        other.connect(timeout=10)
        out["same_frame"] = (other.client.get_transport().get_remote_server_key()
                             == frame.client.get_transport().get_remote_server_key())
        if measure:
            out["speed_usb"] = round(upload_speed(other), 1)
            out["speed_default"] = round(upload_speed(frame), 1)
    finally:
        other.close()
    return out
