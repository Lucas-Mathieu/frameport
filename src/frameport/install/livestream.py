"""Live view: what the Frame shows, played in a browser window on this PC (the GUI's Live view tab).

The Frame sends fragmented MP4 (H.264) on the stdout of one SSH command (`source_command`); a small HTTP server on
127.0.0.1 relays it to a player page (`live_player.py`: Media Source Extensions, no plugin). Flet has no video control
outside packaged builds, so the picture is shown in the browser, which also decodes it in hardware.

The relay keeps the init segment (ftyp + moov) and the fragments since the last keyframe, so a viewer that opens later
starts at a keyframe. No image bytes ever go through Flet.
"""
from __future__ import annotations

import json
import queue
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SAMPLE_NON_SYNC = 0x10000  # ISO/IEC 14496-12 sample_is_non_sync_sample
CLIENT_QUEUE = 240  # fragments a slow viewer may lag behind before it's dropped (it reconnects at a keyframe)


# ------------------------------------------------------------------------------------------------ MP4 parsing
def iter_boxes(data: bytes, start: int = 0, end: int | None = None):
    """(type, payload start, box end) of each complete box in data[start:end]."""
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, pos)
        header = 8
        if size == 1:
            if pos + 16 > end:
                return
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        yield kind.decode("latin-1"), pos + header, pos + size
        pos += size


def _child(data: bytes, start: int, end: int, kind: str):
    for k, s, e in iter_boxes(data, start, end):
        if k == kind:
            return s, e
    return None


def fragment_is_keyframe(moof: bytes) -> bool:
    """Whether a movie fragment starts with a sync sample (trun's first-sample flags, else its per-sample flags,
    else tfhd's default flags). Unknown → False: a viewer then waits for the next fragment that is one."""
    top = _child(moof, 0, len(moof), "moof")
    if not top:
        return False
    for kind, s, e in iter_boxes(moof, *top):
        if kind != "traf":
            continue
        default_flags = None
        tfhd = _child(moof, s, e, "tfhd")
        if tfhd:
            p = tfhd[0]
            flags = int.from_bytes(moof[p + 1:p + 4], "big")
            p += 8  # version/flags + track_ID
            for bit, size in ((0x1, 8), (0x2, 4), (0x8, 4), (0x10, 4)):
                if flags & bit:
                    p += size
            if flags & 0x20 and p + 4 <= tfhd[1]:
                default_flags = struct.unpack_from(">I", moof, p)[0]
        trun = _child(moof, s, e, "trun")
        if not trun:
            continue
        p = trun[0]
        flags = int.from_bytes(moof[p + 1:p + 4], "big")
        count = struct.unpack_from(">I", moof, p + 4)[0]
        p += 8
        if flags & 0x1:
            p += 4  # data_offset
        if flags & 0x4:
            return not struct.unpack_from(">I", moof, p)[0] & SAMPLE_NON_SYNC
        if count and flags & 0x400:
            if flags & 0x100:
                p += 4
            if flags & 0x200:
                p += 4
            return not struct.unpack_from(">I", moof, p)[0] & SAMPLE_NON_SYNC
        if default_flags is not None:
            return not default_flags & SAMPLE_NON_SYNC
        return False
    return False


def codec_string(init: bytes) -> str | None:
    """The MSE codec string (`avc1.PPCCLL`) from the init segment's avcC box."""
    i = init.find(b"avcC")
    if i < 0 or i + 8 > len(init):
        return None
    profile, compat, level = init[i + 5], init[i + 6], init[i + 7]
    return f"avc1.{profile:02x}{compat:02x}{level:02x}"


def video_size(init: bytes) -> tuple[int, int] | None:
    """Width and height from the init segment's avc1 sample entry."""
    i = init.find(b"avc1")
    if i < 0 or i + 4 + 28 > len(init):
        return None
    p = i + 4 + 24  # sample entry: 6 reserved + 2 data ref index + 16 pre_defined/reserved
    return struct.unpack_from(">HH", init, p)


class Mp4Splitter:
    """Cuts a byte stream into the init segment and movie fragments (moof + mdat), whatever the read sizes."""

    def __init__(self):
        self.buf = bytearray()
        self.init = bytearray()
        self.have_init = False
        self._moof: bytes | None = None

    def feed(self, data: bytes) -> list[tuple[str, bytes]]:
        """[("init", bytes) | ("fragment", bytes)] completed by this data."""
        self.buf += data
        out = []
        while True:
            if len(self.buf) < 8:
                break
            size, kind = struct.unpack_from(">I4s", self.buf, 0)
            header = 8
            if size == 1:
                if len(self.buf) < 16:
                    break
                size = struct.unpack_from(">Q", self.buf, 8)[0]
                header = 16
            if size < header:
                raise ValueError(f"bad MP4 box size {size}")
            if len(self.buf) < size:
                break
            box = bytes(self.buf[:size])
            del self.buf[:size]
            kind = kind.decode("latin-1")
            if not self.have_init:
                self.init += box
                if kind == "moov":
                    self.have_init = True
                    out.append(("init", bytes(self.init)))
            elif kind == "moof":
                self._moof = box
            elif kind == "mdat" and self._moof is not None:
                out.append(("fragment", self._moof + box))
                self._moof = None
            # other top-level boxes (styp, sidx, free) carry nothing a viewer needs
        return out


# ------------------------------------------------------------------------------------------------ relay
def _end(q: queue.Queue) -> None:
    """Tell a viewer's sender the stream is over: drop what it hasn't sent yet, then the end marker (None)."""
    try:
        while True:
            q.get_nowait()
    except queue.Empty:
        pass
    try:
        q.put_nowait(None)
    except queue.Full:
        pass


class Relay:
    """Fan-out of one MP4 stream to any number of HTTP viewers."""

    def __init__(self):
        self._lock = threading.Lock()
        self.init: bytes | None = None
        self.gop: list[bytes] = []  # fragments since the last keyframe fragment (incl.)
        self.clients: list[queue.Queue] = []
        self.frames = 0
        self.bytes = 0
        self.started = time.time()
        self.ended: str | None = None  # why the source stopped
        self._splitter = Mp4Splitter()

    def feed(self, data: bytes) -> None:
        for kind, chunk in self._splitter.feed(data):
            with self._lock:
                self.bytes += len(chunk)
                if kind == "init":
                    self.init = chunk
                    continue
                self.frames += 1
                if fragment_is_keyframe(chunk):
                    self.gop = [chunk]
                elif self.gop:
                    self.gop.append(chunk)
                for q in list(self.clients):
                    try:
                        q.put_nowait(chunk)
                    except queue.Full:  # too slow: drop it, its page reconnects (never block: we hold the lock)
                        self.clients.remove(q)
                        _end(q)

    def finish(self, reason: str) -> None:
        with self._lock:
            self.ended = reason
            for q in self.clients:
                _end(q)
            self.clients.clear()

    def subscribe(self) -> tuple[bytes | None, list[bytes], queue.Queue]:
        q: queue.Queue = queue.Queue(CLIENT_QUEUE)
        with self._lock:
            self.clients.append(q)
            return self.init, list(self.gop), q

    def unsubscribe(self, q: queue.Queue) -> None:
        with self._lock:
            if q in self.clients:
                self.clients.remove(q)

    def status(self) -> dict:
        with self._lock:
            init = self.init
            out = {"ready": init is not None, "ended": self.ended, "viewers": len(self.clients),
                   "fragments": self.frames, "bytes": self.bytes, "seconds": round(time.time() - self.started, 1)}
        if init:
            out["codec"] = codec_string(init)
            size = video_size(init)
            if size:
                out["width"], out["height"] = size
        return out


def player_html() -> bytes:
    from .live_player import PLAYER_HTML

    return PLAYER_HTML.encode("utf-8")


class _Handler(BaseHTTPRequestHandler):
    server: _Server
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        live = self.server.live
        if path in ("/", "/index.html"):
            self._send(200, player_html(), "text/html; charset=utf-8")
        elif path == "/status":
            self._send(200, json.dumps(live.relay.status()).encode(), "application/json")
        elif path == "/stream.mp4":
            self._stream(live.relay)
        else:
            self._send(404, b"not found", "text/plain")

    def _stream(self, relay: Relay) -> None:
        if relay.ended:
            self._send(503, relay.ended.encode(), "text/plain")
            return
        init, gop, q = relay.subscribe()
        try:
            deadline = time.time() + 30
            while init is None:  # the source hasn't sent its init segment yet
                if relay.ended or time.time() > deadline:
                    self._send(503, (relay.ended or "no picture yet").encode(), "text/plain")
                    return
                time.sleep(0.1)
                relay.unsubscribe(q)
                init, gop, q = relay.subscribe()
            self.send_response(200)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self._chunk(init)
            for f in gop:
                self._chunk(f)
            while True:
                try:
                    f = q.get(timeout=5)
                except queue.Empty:
                    if relay.ended:
                        break
                    continue
                if f is None:
                    break
                self._chunk(f)
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            relay.unsubscribe(q)

    def _chunk(self, data: bytes) -> None:
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    live: LiveStream


# ------------------------------------------------------------------------------------------------ the Frame's side
# SteamVR on the Frame runs `steamvr-v4l2cam.service` (Valve's v4l2cam): it copies the compositor's "headset view"
# (what the wearer sees: games, SteamVR home, Steam's panels) into a v4l2loopback webcam named "SteamVR"
# (/dev/video99, 1920x1080 RGB24, frames at the display rate). It costs nothing until someone reads it. Steam's own
# recording/Remote Play capture gamescope instead (only the flat Steam UI in VR) and encode with x264 as well; the
# Frame's Qualcomm encoder (qcom-iris) doesn't work with the stock ffmpeg/GStreamer. So: read the headset view, drop
# to 30 fps before scaling, x264 at low priority, fragmented MP4 on stdout. Black while the headset sleeps.
DEVICE_NAME = "SteamVR"
QUALITY = {  # width, height, bitrate
    "720p": (1280, 720, "3M"),
    "1080p": (1920, 1080, "6M"),
}
FPS = 30
ENCODER_THREADS = 3  # leaves the game most of the CPU


def source_command(quality: str = "720p", fps: int = FPS) -> str:
    """Shell script for the Frame: finds the headset-view device, streams fMP4 H.264 to stdout and stops when the SSH
    channel closes (stdin reaches EOF). Exit 3 = no headset view device (SteamVR not running)."""
    w, h, rate = QUALITY.get(quality, QUALITY["720p"])
    scale = "" if (w, h) == (1920, 1080) else f",scale={w}:{h}:flags=fast_bilinear"
    return f"""dev=
for d in /sys/class/video4linux/video*; do
  [ "$(cat "$d/name" 2>/dev/null)" = "{DEVICE_NAME}" ] && dev=/dev/${{d##*/}} && break
done
if [ -z "$dev" ]; then echo "no SteamVR headset view device: is SteamVR running?" >&2; exit 3; fi
exec 3<&0
nice -n 10 ffmpeg -nostdin -hide_banner -loglevel error -f v4l2 -input_format rgb24 -i "$dev" \\
  -vf "fps={fps}{scale},format=yuv420p" -c:v libx264 -preset ultrafast -tune zerolatency -threads {ENCODER_THREADS} \\
  -g {fps} -b:v {rate} -maxrate {rate} -bufsize {rate} -an \\
  -f mp4 -movflags empty_moov+default_base_moof -frag_duration 100000 - &
pid=$!
( cat <&3 >/dev/null; kill $pid 2>/dev/null ) >/dev/null 2>&1 &  # (a background job's own stdin is /dev/null)
wait $pid
"""


class FrameSource:
    """The stdout of `source_command` on the Frame, on its own SSH channel of the existing connection."""

    def __init__(self, frame, quality: str):
        self.frame = frame
        self.quality = quality
        self.chan = None

    def open(self):
        transport = self.frame.client.get_transport()
        if transport is None or not transport.is_active():
            raise ConnectionError("not connected to the Frame")
        self.chan = transport.open_session()
        from ..frame.connection import sh_quote

        self.chan.exec_command("bash -c " + sh_quote(source_command(self.quality)))
        return self

    def read(self, n: int) -> bytes:
        data = self.chan.recv(n)
        if not data:
            err = b""
            while self.chan.recv_stderr_ready():
                err += self.chan.recv_stderr(4096)
            if err.strip():
                raise RuntimeError(err.decode("utf-8", "replace").strip().splitlines()[-1])
        return data

    def close(self) -> None:
        if self.chan is not None:
            try:
                self.chan.shutdown_write()  # EOF on the script's stdin: it stops ffmpeg
            finally:
                self.chan.close()


def start(frame, quality: str = "720p") -> LiveStream:
    """Start streaming the Frame's headset view; open `.url` in a browser to watch."""
    src = FrameSource(frame, quality)
    return LiveStream(src.open, src.close).start()


class LiveStream:
    """One live view: the source command on the Frame + the local relay server.

    `open_source()` returns a file-like object with `read(n)`; it's the stdout of a command on the Frame (or, in tests,
    a local process). `stop()` ends both.
    """

    def __init__(self, open_source, close_source=None, host: str = "127.0.0.1", port: int = 0):
        self._open_source = open_source
        self._close_source = close_source
        self.relay = Relay()
        self.server = _Server((host, port), _Handler)
        self.server.live = self
        self._threads: list[threading.Thread] = []
        self._stop = threading.Event()
        self.on_end = None  # callback(reason) when the source stops by itself

    @property
    def url(self) -> str:
        host, port = self.server.server_address[:2]
        return f"http://{host}:{port}/"

    def start(self) -> LiveStream:
        t1 = threading.Thread(target=self.server.serve_forever, name="live-http", daemon=True)
        t2 = threading.Thread(target=self._pump, name="live-source", daemon=True)
        self._threads = [t1, t2]
        t1.start()
        t2.start()
        return self

    def _pump(self) -> None:
        reason = "stopped"
        try:
            src = self._open_source()
            while not self._stop.is_set():
                data = src.read(65536)
                if not data:
                    reason = "the Frame stopped sending a picture"
                    break
                self.relay.feed(data)
        except Exception as exc:  # noqa: BLE001
            from ..errors import explain

            reason = explain(exc)
        if self._stop.is_set():
            reason = "stopped"
        self.relay.finish(reason)
        if not self._stop.is_set() and self.on_end:
            self.on_end(reason)

    @property
    def running(self) -> bool:
        return not self._stop.is_set() and self.relay.ended is None

    def stop(self) -> None:
        if self._stop.is_set():
            return
        self._stop.set()
        if self._close_source:
            try:
                self._close_source()
            except Exception:  # noqa: BLE001
                pass
        self.relay.finish("stopped")
        self.server.shutdown()
        self.server.server_close()
