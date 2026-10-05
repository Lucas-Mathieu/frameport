#!/usr/bin/env python3
# ruff: noqa: E501  (the shell snippet for the Frame keeps its lines whole)
"""Autonomous USB-cable test (Frame cabled to this PC): everything needed to decide on a "Connect with a USB cable"
setup option, in one run, without touching the headset. Writes a Markdown report.

    uv run python scripts/usb_autotest.py --out <report.md> [--no-speed]

Checks: Windows (via powershell.exe from WSL, or directly): new network adapters, Valve USB devices (VID 28DE), the
driver Windows bound and the USB class codes (NCM 02/0D, ECM 02/06, RNDIS E0 or EF/04); the adapter's IPv4 and
network category; Frame side (over the normal connection): agent `usb_link`, the systemd units / NetworkManager
connection behind usb0 (does it depend on Developer Mode?); Frame -> PC over the cable (the setup direction: a
temporary listener on a pairing port); PC -> Frame SSH over the cable (same host key); upload speed USB vs normal.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

PS_PATHS = ("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "powershell.exe", "powershell")
LISTEN_PORT = 8766  # in FramePort's pairing range (8765-8767)


def powershell() -> str | None:
    for p in PS_PATHS:
        if os.path.exists(p) or shutil.which(p):
            return p
    return None


def ps(script: str, timeout: int = 60) -> str:
    exe = powershell()
    if not exe:
        return "(no PowerShell)"
    env = {k: v for k, v in os.environ.items() if k != "PSModulePath"}
    try:
        p = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True,
                           text=True, timeout=timeout, env=env)
        return (p.stdout + p.stderr).replace("\r", "").strip()
    except (OSError, subprocess.SubprocessError) as exc:
        return f"(powershell failed: {exc})"


WINDOWS = r"""
$ErrorActionPreference = 'SilentlyContinue'
'== adapters'
Get-NetAdapter -IncludeHidden | Select-Object Name,InterfaceDescription,Status,LinkSpeed,MacAddress |
  Format-Table -AutoSize | Out-String -Width 250
'== Valve USB devices (VID 28DE)'
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_28DE' } | ForEach-Object {
  $d = $_
  $compat = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_CompatibleIds').Data -join ' '
  $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service').Data
  $drv = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_DriverDesc').Data
  "{0} | {1} | status={2} | class={3} | service={4} | driver={5} | compat={6}" -f $d.FriendlyName, $d.InstanceId,
    $d.Status, $d.Class, $svc, $drv, $compat
}
'== problem devices'
Get-PnpDevice -PresentOnly | Where-Object { $_.Status -ne 'OK' -and $_.InstanceId -match 'USB' } |
  Select-Object FriendlyName,Status,InstanceId | Format-Table -AutoSize | Out-String -Width 250
'== IPv4 on 10.86.200.*'
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '10.86.200.*' } |
  Select-Object InterfaceAlias,IPAddress,PrefixLength,PrefixOrigin | Format-Table | Out-String
'== network profiles'
Get-NetConnectionProfile | Select-Object InterfaceAlias,NetworkCategory | Format-Table | Out-String
'== SSH to the Frame over the cable (from Windows)'
(Test-NetConnection 10.86.200.233 -Port 22 -WarningAction SilentlyContinue).TcpTestSucceeded
'== FramePort firewall rules'
Get-NetFirewallRule -DisplayName '*FramePort*' | Select-Object DisplayName,Enabled,Profile,Direction |
  Format-Table | Out-String
"""

FRAME_UNITS = r"""
echo "== units mentioning usb/gadget"; systemctl list-units --all --no-pager 2>/dev/null | grep -i -E 'gadget|usb' | head -20
for u in $(systemctl list-unit-files --no-pager 2>/dev/null | grep -i -E 'gadget|usb' | awk '{print $1}' | head -8); do
  echo "== $u"; systemctl cat "$u" --no-pager 2>/dev/null | grep -v '^#' | grep -v '^$' | head -30; done
echo "== NetworkManager usb0"; nmcli -f GENERAL.CONNECTION,IP4.ADDRESS device show usb0 2>/dev/null
c=$(nmcli -g GENERAL.CONNECTION device show usb0 2>/dev/null); [ -n "$c" ] && nmcli -f ipv4.method,ipv4.addresses,connection.autoconnect connection show "$c" 2>/dev/null
echo "== configfs"; ls -la /sys/kernel/config/usb_gadget/ 2>&1 | head; ls /sys/kernel/config/usb_gadget/*/functions/ 2>&1 | head
echo "== udc"; for u in /sys/class/udc/*; do echo "$u $(cat $u/current_speed 2>/dev/null) $(cat $u/state 2>/dev/null)"; done
echo "== devkit"; ls -la /etc/steamos-devkit-enabled 2>&1
"""


def frame_to_pc(frame, pc_ips: list[str]) -> list[str]:
    """Can the Frame open a TCP connection to this PC over the cable (what the setup step needs)?"""
    lines, got = [], []
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", LISTEN_PORT))
    except OSError as exc:
        return [f"couldn't listen on {LISTEN_PORT}: {exc}"]
    srv.listen(4)
    srv.settimeout(1)

    def accept():
        end = time.time() + 20
        while time.time() < end:
            try:
                conn, addr = srv.accept()
                got.append(addr[0])
                conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nframeport-usb-ok\n")
                conn.close()
            except OSError:
                continue
    t = threading.Thread(target=accept, daemon=True)
    t.start()
    for ip in pc_ips:
        code, out, err = frame.run(f"curl -s --max-time 6 http://{ip}:{LISTEN_PORT}/ping || echo FAILED", timeout=20)
        lines.append(f"Frame -> {ip}:{LISTEN_PORT}: {(out or err).strip()[:80]}")
    t.join(timeout=22)
    srv.close()
    lines.append(f"connections seen from: {got or 'none'}")
    return lines


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("usb-autotest.md"))
    ap.add_argument("--no-speed", action="store_true")
    args = ap.parse_args()
    from frameport.frame import usb
    from frameport.frame.connection import saved_targets
    from frameport.targets.frame_lepton import FrameLeptonTarget

    report = [f"# USB cable autotest {time.strftime('%Y-%m-%d %H:%M')}", ""]

    def section(title, text):
        report.extend([f"## {title}", "```", text.strip() or "(nothing)", "```", ""])
        print(f"--- {title}\n{text.strip()[:3000]}\n", flush=True)

    section("Windows", ps(WINDOWS, timeout=120))
    wsl = subprocess.run(["ip", "-4", "-o", "addr", "show"], capture_output=True, text=True).stdout
    section("This side (WSL/Linux) IPv4", wsl)
    frame = FrameLeptonTarget(saved_targets()[0], None).connect().frame
    section("Frame units / NetworkManager / configfs", frame.run(FRAME_UNITS, timeout=60)[1])
    result = usb.check(frame, measure=not args.no_speed)
    section("frameport frame usb-check", json.dumps(result, indent=1, default=str))
    ips = result.get("pc_addresses") or []
    section("Frame -> PC over the cable (setup direction)", "\n".join(frame_to_pc(frame, ips)) if ips else
            "skipped: this side has no address on the Frame's USB network")
    args.out.write_text("\n".join(report), encoding="utf-8")
    print(f"report: {args.out}")


if __name__ == "__main__":
    main()
