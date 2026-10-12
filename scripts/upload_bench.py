"""Upload speed to the Frame by method (GitHub discussion #242: paramiko capped at ~23 MB/s on a reporter's PC).

    uv run python scripts/upload_bench.py [--mib 512] [--host <addr>] [--methods app,paramiko-tuned,ssh,ssh-gcm,sftp]

Methods, each sending the same random file to ~/frameport-bench on the Frame (removed afterwards):
- app             FramePort's own upload (Frame.put: pipelined SFTP, 1 MiB writes)
- paramiko-tuned  the same through a paramiko transport/SFTP with a 64 MiB window
- ssh             the system's OpenSSH: `ssh … "cat > file"` (default cipher)
- ssh-gcm         the same with aes128-gcm@openssh.com
- ssh-chacha      the same with chacha20-poly1305@openssh.com
- sftp            OpenSSH's sftp in batch mode
Prints MB/s, wall time and the CPU time spent on this PC (this process + children). Dev tool, not part of the app.
"""
from __future__ import annotations

import argparse
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import paramiko  # noqa: E402

from frameport.frame.connection import Frame, app_key, saved_targets, ssh_dir  # noqa: E402

REMOTE = "frameport-bench"


def cpu() -> float:
    t = os.times()
    return t.user + t.system + t.children_user + t.children_system


def private_key() -> str:
    """OpenSSH refuses a key file others can read (the Windows data folder under /mnt/c shows 0777): a 0600 copy."""
    key = ssh_dir() / "id_ed25519"
    if os.name == "nt" or stat.S_IMODE(key.stat().st_mode) & 0o077 == 0:
        return str(key)
    tmp = Path(tempfile.mkdtemp()) / "id_ed25519"
    shutil.copy(key, tmp)
    tmp.chmod(0o600)
    return str(tmp)


def ssh_base(frame: Frame, key: str, port_flag: str = "-p") -> list[str]:
    return ["-i", key, "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
            "-o", f"UserKnownHostsFile={ssh_dir() / 'known_hosts'}", "-o", "StrictHostKeyChecking=yes",
            port_flag, str(frame.target.port)]


def m_app(frame, local, remote, key):
    frame.put(local, remote, resume=False)


def m_paramiko_tuned(frame, local, remote, key):
    sock = socket.create_connection((frame.target.host, frame.target.port), timeout=10)
    t = paramiko.Transport(sock, default_window_size=64 << 20, default_max_packet_size=32768)
    try:
        t.connect(username=frame.target.user, pkey=app_key())
        sftp = paramiko.SFTPClient.from_transport(t, window_size=64 << 20, max_packet_size=32768)
        with open(local, "rb") as src, sftp.open(remote, "wb") as dst:
            dst.set_pipelined(True)
            while chunk := src.read(4 << 20):
                dst.write(chunk)
    finally:
        t.close()


def _ssh_cat(cipher):
    def run(frame, local, remote, key):
        extra = ["-c", cipher] if cipher else []
        with open(local, "rb") as src:
            subprocess.run(["ssh", *ssh_base(frame, key), *extra, f"{frame.target.user}@{frame.target.host}",
                            f"cat > {remote}"], stdin=src, check=True)
    return run


def m_sftp(frame, local, remote, key):
    subprocess.run(["sftp", *ssh_base(frame, key, "-P"), "-b", "-", f"{frame.target.user}@{frame.target.host}"],
                   input=f'put "{local}" "{remote}"\n'.encode(), check=True, stdout=subprocess.DEVNULL)


METHODS = {"app": m_app, "paramiko-tuned": m_paramiko_tuned, "ssh": _ssh_cat(None),
           "ssh-gcm": _ssh_cat("aes128-gcm@openssh.com"), "ssh-chacha": _ssh_cat("chacha20-poly1305@openssh.com"),
           "sftp": m_sftp}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mib", type=int, default=512)
    ap.add_argument("--host", help="the Frame's address (default: the remembered Frame)")
    ap.add_argument("--methods", default="app,paramiko-tuned,ssh,ssh-gcm,ssh-chacha,sftp")
    a = ap.parse_args()
    target = saved_targets()[0]
    if a.host:
        target.host = a.host
    frame = Frame(target).connect()
    remote = f"{frame.home}/{REMOTE}"
    key = private_key()
    with tempfile.NamedTemporaryFile(delete=False) as f:
        block = os.urandom(1 << 20)
        for _ in range(a.mib):
            f.write(block)
        local = f.name
    size = a.mib << 20
    print(f"{a.mib} MiB to {target.host} ({sys.platform}, paramiko {paramiko.__version__})")
    try:
        for name in a.methods.split(","):
            if name.startswith(("ssh", "sftp")) and not shutil.which(name.split("-")[0]):
                print(f"{name:15} skipped: no {name.split('-')[0]} on PATH")
                continue
            c0, t0 = cpu(), time.monotonic()
            try:
                METHODS[name](frame, local, remote, key)
            except Exception as exc:  # noqa: BLE001
                print(f"{name:15} failed: {exc}")
                continue
            wall, used = time.monotonic() - t0, cpu() - c0
            got = int(frame.run(f"stat -c %s {remote}")[1].strip() or 0)
            frame.run(f"rm -f {remote}")
            ok = "" if got == size else f"  (size {got} != {size})"
            print(f"{name:15} {size / wall / 1e6:7.1f} MB/s  {wall:6.1f} s  CPU here {used:5.1f} s{ok}")
    finally:
        os.remove(local)
    return 0


if __name__ == "__main__":
    sys.exit(main())
