"""Pairing server: while the Setup page is open, serve the bootstrap script and FramePort's public key over HTTP on
the LAN. The user types one line on the Frame; the script calls back /paired so the UI knows who connected.

Every request needs the pairing code, a random secret that is only in that one line. The server stops after a
successful pairing, after MAX_FAILURES wrong codes (someone guessing) and after LIFETIME seconds, so a short code
(32 bits) is plenty. The line is typed by hand on the Frame, so it's kept short: no scheme (curl defaults to http),
the code is the path, and a fixed port when it's free."""
from __future__ import annotations

import http.server
import secrets
import threading
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

from ..core.paths import bootstrap_dir
from .connection import app_public_key
from .discovery import local_ip_towards

MAX_FAILURES = 20
LIFETIME = 30 * 60
PORTS = (8765, 8766, 8767, 0)  # 0 = any free port
FIREWALL_RULE = "FramePort-Pairing"
HINT_AFTER = 45  # seconds without any request from the Frame before the UI suggests what may block it


def ensure_reachable(server: PairingServer) -> str:
    """Under WSL, let the Frame reach the pairing ports through Windows' Hyper-V firewall while `server` runs (one
    admin prompt; the rule is removed again when the server stops, see winhost.open_wsl_inbound).
    Returns "ok" (nothing blocks), "opened" or "failed" (declined / no admin)."""
    from ..core import winhost

    if not winhost.wsl_inbound_blocked(FIREWALL_RULE):
        return "ok"
    temp = winhost.env_path("TEMP")
    if temp is None:
        return "failed"
    server.flag = temp / f"frameport-pairing-{server.code}.flag"
    try:
        server.flag.write_text("FramePort's setup page is open: the firewall rule stays while this file exists\n")
    except OSError:
        return "failed"
    ports = f"{PORTS[0]}-{PORTS[-2]}"
    return "opened" if winhost.open_wsl_inbound(FIREWALL_RULE, "FramePort setup (WSL, temporary)", ports,
                                                server.flag) else "failed"


def firewall_hint(port: int) -> str:
    """What may keep the Frame from reaching this PC, for this OS (shown when no request came in after HINT_AFTER).
    Empty when nothing specific is known."""
    import platform
    import shutil
    import subprocess

    from ..core import winhost
    from ..i18n import tr

    def out(*cmd) -> str:
        try:
            return subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout.lower()
        except (OSError, subprocess.SubprocessError):
            return ""

    if winhost.is_wsl():
        if winhost.wsl_networking_mode() == "nat":
            return tr("FramePort runs in WSL with its default NAT network, which other devices can't reach. Set "
                      "networkingMode=mirrored under [wsl2] in %UserProfile%\\.wslconfig, run wsl --shutdown and "
                      "start FramePort again.")
        if winhost.wsl_inbound_blocked(FIREWALL_RULE):
            return tr("Windows' firewall for WSL blocks the Frame. Open the setup command again and allow the "
                      "change when Windows asks (admin).")
        return ""
    if winhost.is_windows():
        if winhost.network_category(local_ip_towards()) == "Public":
            return tr("Windows treats this network as public and its firewall blocks the Frame. In Windows' "
                      "network settings set this network to Private (or allow FramePort on public networks when "
                      "Windows asks).")
        return tr("If Windows asked whether FramePort may use the network, allow it (Private networks).")
    if platform.system() == "Darwin":
        fw = out("/usr/libexec/ApplicationFirewall/socketfilterfw", "--getglobalstate")
        if "enabled" in fw:
            return tr("macOS' firewall is on: allow incoming connections when macOS asks about FramePort (or "
                      "System Settings → Network → Firewall → Options).")
        return ""
    if shutil.which("firewall-cmd") and "running" in out("firewall-cmd", "--state"):
        return tr("firewalld is active. Allow the setup port until the next restart with: "
                  "sudo firewall-cmd --add-port={port}/tcp").format(port=port)
    if "active" == out("systemctl", "is-active", "ufw").strip():
        return tr("ufw is active. Allow the setup port with: sudo ufw allow {port}/tcp (and afterwards: "
                  "sudo ufw delete allow {port}/tcp)").format(port=port)
    return ""


@dataclass
class PairingServer:
    port: int = 0
    code: str = field(default_factory=lambda: secrets.token_hex(4))
    failures: int = 0
    paired: list[dict] = field(default_factory=list)
    on_paired: object = None
    requests: int = 0  # requests from the network (right or wrong code): the Frame can reach us
    flag: Path | None = None  # WSL: the temporary firewall rule stays while this file exists
    hint: str = ""  # set by the UI when nothing reached us after HINT_AFTER seconds: what may block the Frame
    host: str = ""  # the address the Frame uses to reach this PC; "" = the PC's address towards the internet. The USB
    # setup uses the cable's fixed PC address (10.86.200.234), so no Wi-Fi, router or discovery is involved
    _httpd: http.server.ThreadingHTTPServer | None = None

    @property
    def url(self) -> str:
        return f"http://{self.host or local_ip_towards()}:{self.port}"

    @property
    def one_liner(self) -> str:
        return f"curl -fsS {self.url.removeprefix('http://')}/{self.code} | bash"

    def start(self) -> PairingServer:
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, body: bytes, ctype="text/plain", status=200):
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                server.requests += 1
                url = urllib.parse.urlparse(self.path)
                q = dict(urllib.parse.parse_qsl(url.query))
                path, code = url.path, q.get("code", "")
                if not code and path.count("/") == 1:  # the short form: /<code> = the script
                    path, code = "/bootstrap.sh", path[1:]
                if not secrets.compare_digest(code, server.code):
                    server.failures += 1
                    if server.failures >= MAX_FAILURES:
                        server.stop_soon()
                    return self._send(b"wrong or missing pairing code\n", status=403)
                if path == "/bootstrap.sh":
                    text = (bootstrap_dir() / "bootstrap.sh").read_text()
                    text = text.replace("__PC_URL__", server.url).replace("__PAIR_CODE__", server.code)
                    return self._send(text.encode(), "text/x-shellscript")
                if path == "/key":
                    return self._send((app_public_key() + "\n").encode())
                if path == "/paired":
                    info = {"host": self.client_address[0], "user": q.get("user", "steamos"), "name": q.get("host", "")}
                    server.paired.append(info)
                    if callable(server.on_paired):
                        server.on_paired(info)
                    server.stop_soon()  # paired: nothing else to serve
                    return self._send(b"ok\n")
                return self._send(b"not found\n", status=404)

        for port in ((self.port,) if self.port else PORTS):
            try:
                self._httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
                break
            except OSError:
                if port == PORTS[-1]:
                    raise
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        self._timer = threading.Timer(LIFETIME, self.stop)
        self._timer.daemon = True
        self._timer.start()
        return self

    @property
    def running(self) -> bool:
        return self._httpd is not None

    def stop_soon(self) -> None:
        """Stop from inside a request handler (shutdown() waits for the serving thread, so not from that thread)."""
        threading.Thread(target=self.stop, daemon=True).start()

    def stop(self) -> None:
        httpd, self._httpd = self._httpd, None
        if httpd:
            httpd.shutdown()
            httpd.server_close()
        timer = getattr(self, "_timer", None)
        if timer:
            timer.cancel()
        if self.flag is not None:  # lets the temporary firewall rule go
            self.flag.unlink(missing_ok=True)
