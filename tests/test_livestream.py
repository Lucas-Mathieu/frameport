"""Live view: the fragmented-MP4 relay (box splitting, keyframe detection, late viewers), the player page, and the
Frame-side script (device lookup, stops when the SSH channel closes)."""
import io
import shutil
import struct
import subprocess
import time
import urllib.request

import pytest

from frameport.install import livestream as L


def box(kind: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", 8 + len(payload), kind.encode()) + payload


def full(kind: str, flags: int, payload: bytes) -> bytes:
    return box(kind, struct.pack(">I", flags) + payload)  # version 0


def init_segment(w=1280, h=720) -> bytes:
    # avc1 sample entry: 6 reserved + 2 data ref + 16 pre_defined/reserved, then width/height; avcC: version,
    # profile 0x42, compat 0xc0, level 0x1f
    avcc = box("avcC", bytes([1, 0x42, 0xC0, 0x1F]) + b"\0" * 4)
    avc1 = box("avc1", b"\0" * 24 + struct.pack(">HH", w, h) + b"\0" * 50 + avcc)
    return box("ftyp", b"isom\0\0\0\0") + box("moov", box("trak", avc1))


def fragment(key: bool, how: str = "first", seq: int = 0) -> bytes:
    flags = 0 if key else L.SAMPLE_NON_SYNC
    if how == "first":  # trun first_sample_flags (what ffmpeg writes)
        trun = full("trun", 0x1 | 0x4, struct.pack(">IiI", 2, 0, flags))
        tfhd = full("tfhd", 0, struct.pack(">I", 1))
    elif how == "sample":  # per-sample flags
        trun = full("trun", 0x400, struct.pack(">II", 1, flags))
        tfhd = full("tfhd", 0, struct.pack(">I", 1))
    else:  # tfhd default flags
        trun = full("trun", 0, struct.pack(">I", 1))
        tfhd = full("tfhd", 0x20, struct.pack(">II", 1, flags))
    return box("moof", box("mfhd", struct.pack(">II", 0, seq)) + box("traf", tfhd + trun)) + \
        box("mdat", bytes([seq % 256]) * 32)


@pytest.mark.parametrize("how", ["first", "sample", "default"])
def test_keyframe_detection(how):
    assert L.fragment_is_keyframe(fragment(True, how))
    assert not L.fragment_is_keyframe(fragment(False, how))


def test_keyframe_unknown_is_not_key():
    assert not L.fragment_is_keyframe(box("moof", box("mfhd", b"\0" * 8)))
    assert not L.fragment_is_keyframe(b"junk")


def test_codec_and_size():
    init = init_segment(1920, 1080)
    assert L.codec_string(init) == "avc1.42c01f"
    assert L.video_size(init) == (1920, 1080)
    assert L.codec_string(b"nothing") is None


def test_splitter_any_read_size():
    stream = init_segment() + fragment(True, seq=1) + fragment(False, seq=2) + box("free", b"x")
    for step in (1, 7, 64, len(stream)):
        sp, out = L.Mp4Splitter(), []
        for i in range(0, len(stream), step):
            out += sp.feed(stream[i:i + step])
        assert [k for k, _ in out] == ["init", "fragment", "fragment"]
        assert out[0][1] == init_segment()
        assert out[1][1] == fragment(True, seq=1)


def test_splitter_rejects_garbage():
    with pytest.raises(ValueError):
        L.Mp4Splitter().feed(struct.pack(">I4s", 3, b"bad!"))


def test_relay_late_viewer_starts_at_keyframe():
    r = L.Relay()
    r.feed(init_segment() + fragment(True, seq=1) + fragment(False, seq=2) + fragment(True, seq=3)
           + fragment(False, seq=4))
    init, gop, q = r.subscribe()
    assert init == init_segment()
    assert gop == [fragment(True, seq=3), fragment(False, seq=4)]
    r.feed(fragment(False, seq=5))
    assert q.get_nowait() == fragment(False, seq=5)
    st = r.status()
    assert st["ready"] and st["codec"] == "avc1.42c01f" and st["width"] == 1280 and st["viewers"] == 1
    r.finish("gone")
    assert q.get_nowait() is None and r.status()["ended"] == "gone"


def test_relay_drops_slow_viewer(monkeypatch):
    monkeypatch.setattr(L, "CLIENT_QUEUE", 2)
    r = L.Relay()
    r.feed(init_segment())
    _, _, q = r.subscribe()
    for i in range(3):
        r.feed(fragment(True, seq=i))
    assert r.status()["viewers"] == 0  # dropped; its page reconnects at a keyframe


class _Source:
    """A byte stream fed at the pace a test chooses."""

    def __init__(self, data: bytes):
        self.f = io.BytesIO(data)

    def read(self, n):
        time.sleep(0.01)
        return self.f.read(min(n, 200))


def test_server_serves_page_status_and_stream():
    data = init_segment() + b"".join(fragment(i % 3 == 0, seq=i) for i in range(6))
    live = L.LiveStream(lambda: _Source(data)).start()
    try:
        page = urllib.request.urlopen(live.url, timeout=5).read()
        assert b"MediaSource" in page and b"stream.mp4" in page
        deadline = time.time() + 5
        while live.relay.ended is None and time.time() < deadline:
            time.sleep(0.05)
        import json

        st = json.loads(urllib.request.urlopen(live.url + "status", timeout=5).read())
        assert st["ready"] and st["fragments"] == 6 and st["ended"]
        with pytest.raises(urllib.error.HTTPError):  # source over: no stream to join (503 isn't hung)
            urllib.request.urlopen(live.url + "stream.mp4", timeout=5).read()
    finally:
        live.stop()
    assert not live.running


def test_stream_endpoint_sends_init_then_gop():
    gate = []

    class Slow:
        def __init__(self):
            self.parts = [init_segment(), fragment(True, seq=1), fragment(False, seq=2)]

        def read(self, n):
            if self.parts:
                return self.parts.pop(0)
            while not gate:
                time.sleep(0.02)
            return b""

    live = L.LiveStream(Slow).start()
    try:
        deadline = time.time() + 5
        while live.relay.status()["fragments"] < 2 and time.time() < deadline:
            time.sleep(0.02)
        resp = urllib.request.urlopen(live.url + "stream.mp4", timeout=5)
        assert resp.headers["Content-Type"] == "video/mp4"
        gate.append(1)
        body = resp.read()
        assert body == init_segment() + fragment(True, seq=1) + fragment(False, seq=2)
    finally:
        live.stop()


def test_source_command_scales_down_only():
    c720 = L.source_command("720p")
    assert "scale=-2:'min(720,ih)'" in c720 and "fps=30" in c720 and '"SteamVR"' in c720
    assert c720.index("fps=30") < c720.index("scale=")  # drop frames before the expensive scale
    for q, (h, _) in L.QUALITY.items():
        assert ("scale=" in L.source_command(q)) == (h is not None)
    assert L.source_command("bogus") == L.source_command(L.DEFAULT_QUALITY)


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_source_command_without_device_exits_3(tmp_path):
    script = L.source_command().replace("/sys/class/video4linux", str(tmp_path))
    p = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=10, stdin=subprocess.DEVNULL)
    assert p.returncode == 3 and "SteamVR" in p.stderr


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_source_command_stops_when_stdin_closes(tmp_path):
    """The SSH channel closing = EOF on stdin: the encoder must stop (it would keep the Frame busy)."""
    dev = tmp_path / "video99"
    dev.mkdir()
    (dev / "name").write_text("SteamVR\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "ffmpeg"
    fake.write_text("#!/bin/sh\nexec sleep 30\n")
    fake.chmod(0o755)
    script = L.source_command().replace("/sys/class/video4linux", str(tmp_path))
    p = subprocess.Popen(["bash", "-c", script], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         env={"PATH": f"{bindir}:/usr/bin:/bin"})
    time.sleep(0.5)
    assert p.poll() is None  # streaming
    p.stdin.close()
    assert p.wait(timeout=5) is not None


def trak(track_id: int, handler: bytes, entry: bytes) -> bytes:
    tkhd = full("tkhd", 3, b"\0" * 8 + struct.pack(">I", track_id) + b"\0" * 68)
    hdlr = full("hdlr", 0, b"\0" * 4 + handler + b"\0" * 13)
    return box("trak", tkhd + box("mdia", hdlr + box("minf", box("stbl", box("stsd", entry)))))


def av_init() -> bytes:
    avc1 = box("avc1", b"\0" * 24 + struct.pack(">HH", 1280, 720) + b"\0" * 50
               + box("avcC", bytes([1, 0x42, 0xC0, 0x1F]) + b"\0" * 4))
    return box("ftyp", b"isom\0\0\0\0") + box("moov", trak(2, b"soun", box("mp4a", b"\0" * 28))
                                                 + trak(1, b"vide", avc1))


def av_fragment(video_key: bool, seq: int) -> bytes:
    """Audio traf first (all sync samples), then the video traf."""
    def traf(tid, flags):
        return box("traf", full("tfhd", 0, struct.pack(">I", tid)) + full("trun", 0x4, struct.pack(">II", 1, flags)))
    return box("moof", box("mfhd", struct.pack(">II", 0, seq)) + traf(2, 0)
               + traf(1, 0 if video_key else L.SAMPLE_NON_SYNC)) + box("mdat", b"x" * 8)


def test_audio_track_codecs_and_video_track():
    init = av_init()
    assert L.codec_string(init) == "avc1.42c01f, mp4a.40.2"
    assert L.video_track_id(init) == 1
    assert L.video_track_id(init_segment()) is None  # (the minimal helper has no tkhd/hdlr)


def test_keyframe_looks_at_video_track_only():
    assert not L.fragment_is_keyframe(av_fragment(False, 1), 1)  # the audio traf's sync sample doesn't count
    assert L.fragment_is_keyframe(av_fragment(True, 1), 1)
    assert not L.fragment_is_keyframe(av_fragment(True, 1), 7)  # no such track


def test_relay_with_audio():
    r = L.Relay()
    r.feed(av_init() + av_fragment(True, 1) + av_fragment(False, 2) + av_fragment(False, 3))
    _, gop, _ = r.subscribe()
    assert len(gop) == 3  # the non-key fragments with audio didn't restart the group
    st = r.status()
    assert st["audio"] and st["codec"].endswith("mp4a.40.2")


def test_source_command_captures_default_output():
    cmd = L.source_command()
    assert "pactl get-default-sink" in cmd and "$sink.monitor" in cmd and "-c:a aac" in cmd
    audio = cmd[cmd.index("audio=(-thread_queue_size"):cmd.index("fi\n", cmd.index("audio=(-thread_queue_size"))]
    assert "-ts mono2abs" in cmd and "-use_wallclock_as_timestamps" not in audio  # both on pulse's wall clock
    # (the hardware path stamps its raw H.264 input with the wall clock; the pulse input never gets that flag)
    assert "aresample=async=1" in cmd


# ------------------------------------------------------------------------------------------------ hardware encoder
def _fake_tools(tmp_path, probe_exit=0, fps=32):
    """A headset-view device, a fake fp_venc (probe JSON / copies stdin to stdout) and a fake ffmpeg (logs its args,
    reads its stdin to the end)."""
    dev = tmp_path / "video99"
    dev.mkdir()
    (dev / "name").write_text("SteamVR\n")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    venc = bindir / "fp_venc"
    venc.write_text("#!/bin/sh\n"
                    f'echo "$@" >> {tmp_path}/venc.args\n'
                    'case " $* " in *" --probe "*)\n'
                    f'  [ {probe_exit} = 0 ] || exit {probe_exit}\n'
                    f'  echo \'{{"encoder":"/dev/video23","fps":{fps},"bitrate":3000000}}\'; exit 0;;\n'
                    "esac\n"
                    "exec cat\n")
    ffmpeg = bindir / "ffmpeg"
    ffmpeg.write_text("#!/bin/sh\n"
                      f'echo "$@" > {tmp_path}/ffmpeg.args\n'
                      "exec cat > /dev/null\n")
    for f in (venc, ffmpeg):
        f.chmod(0o755)
    env = {"PATH": f"{bindir}:/usr/bin:/bin", "FP_VENC": str(venc)}
    return env


def _run_script(tmp_path, env, quality="720p"):
    script = L.source_command(quality).replace("/sys/class/video4linux", str(tmp_path))
    return subprocess.Popen(["bash", "-c", script], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env)


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_source_command_hardware_path(tmp_path):
    p = _run_script(tmp_path, _fake_tools(tmp_path, fps=32))
    p.stdin.write(b"k")  # a keyframe request reaches the encoder (here: copied on to ffmpeg)
    p.stdin.flush()
    time.sleep(0.5)
    assert p.poll() is None
    p.stdin.close()  # the SSH channel closing ends the encoder, then ffmpeg
    assert p.wait(timeout=5) == 0
    assert b"live: encoder=hardware fps=32" in p.stderr.read()
    args = (tmp_path / "ffmpeg.args").read_text()
    assert "-f h264 -i pipe:0" in args and "-c:v copy" in args and "libx264" not in args
    assert "-use_wallclock_as_timestamps 1" in args and "nobuffer" not in args  # read in step with pulse's audio
    assert "setts=ts=N*(1/32)/TB" in args  # output timestamps on fp_venc's 32 fps grid (no bunched frames)
    assert "-itsoffset -0.25 -f h264 -i pipe:0" in args  # video not "ahead" of pulse, not held for interleaving
    assert "+frag_keyframe" in args  # a requested keyframe starts a fragment (where a new viewer begins)
    venc_calls = (tmp_path / "venc.args").read_text().splitlines()
    assert venc_calls[0].startswith("--probe ") and "--height 720" in venc_calls[0]
    assert "--bitrate 5000000 --peak 7500000" in venc_calls[0]  # hardware 720p: VBR, 1.5x peak
    assert "--fps 32" in venc_calls[1] and "--probe" not in venc_calls[1]


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_source_command_full_quality_has_no_height(tmp_path):
    p = _run_script(tmp_path, _fake_tools(tmp_path), quality="full")
    p.stdin.close()
    p.wait(timeout=5)
    assert "--height" not in (tmp_path / "venc.args").read_text()


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_source_command_falls_back_to_x264(tmp_path):
    p = _run_script(tmp_path, _fake_tools(tmp_path, probe_exit=4))  # hardware encoder not usable
    time.sleep(0.5)
    p.stdin.close()
    p.wait(timeout=5)
    assert b"live: encoder=software fps=30" in p.stderr.read()
    args = (tmp_path / "ffmpeg.args").read_text()
    assert "libx264" in args and "-f v4l2" in args
    assert len((tmp_path / "venc.args").read_text().splitlines()) == 1  # only the probe ran


def test_bitrate_bps():
    assert [L.bitrate_bps(r) for _, r in L.QUALITY.values()] == [1_000_000, 1_500_000, 3_000_000, 6_000_000,
                                                                 10_000_000]


def test_relay_viewer_waits_for_requested_keyframe():
    r = L.Relay()
    asked = []
    r.on_join = lambda: asked.append(1) or True
    r.feed(init_segment() + fragment(True, seq=1) + fragment(False, seq=2))
    init, gop, q = r.subscribe()
    assert asked and init == init_segment() and gop == []  # no stale group: the requested keyframe comes next
    r.feed(fragment(False, seq=3))  # still before the keyframe: not for this viewer
    r.feed(fragment(True, seq=4) + fragment(False, seq=5))
    assert q.get_nowait() == fragment(True, seq=4)
    assert q.get_nowait() == fragment(False, seq=5)


def test_relay_without_keyframe_request_sends_gop():
    r = L.Relay()
    r.on_join = lambda: False  # x264 path: no way to ask
    r.feed(init_segment() + fragment(True, seq=1) + fragment(False, seq=2))
    _, gop, q = r.subscribe()
    assert gop == [fragment(True, seq=1), fragment(False, seq=2)]
    r.feed(fragment(False, seq=3))
    assert q.get_nowait() == fragment(False, seq=3)


class _Chan:
    def __init__(self, data: bytes, err: bytes):
        self.data, self.err, self.sent = [data, b""], err, b""

    def recv(self, n):
        return self.data.pop(0) if self.data else b""

    def recv_stderr_ready(self):
        return bool(self.err)

    def recv_stderr(self, n):
        out, self.err = self.err[:n], self.err[n:]
        return out

    def sendall(self, b):
        self.sent += b


def test_frame_source_reads_info_and_errors():
    src = L.FrameSource(frame=None, quality="720p")
    src.chan = _Chan(b"mp4", b"live: encoder=hardware fps=36\nfp_venc: stats frames=1\nffmpeg: broken pipe")
    assert not src.request_keyframe()  # nothing known yet
    assert src.read(10) == b"mp4"
    assert src.info == {"encoder": "hardware", "fps": "36"}
    assert src.request_keyframe() and src.chan.sent == b"k"
    with pytest.raises(RuntimeError, match="broken pipe"):  # the last stderr line explains the end
        src.read(10)
    src = L.FrameSource(frame=None, quality="720p")  # encoder stats only go to the log; an encoder error explains
    src.chan = _Chan(b"", b"fp_venc: encoder QBUF (OUTPUT) failed, errno 22\nfp_venc: stats frames=9 late=0\n")
    with pytest.raises(RuntimeError, match="QBUF"):
        src.read(10)


def test_live_stream_status_has_encoder():
    live = L.LiveStream(lambda: _Source(b""))
    try:
        live.source_info = lambda: {"encoder": "hardware", "fps": "32"}
        st = live.status()
        assert st["encoder"] == "hardware" and st["fps"] == 32
    finally:
        live.server.server_close()


class _Frame:
    home = "/home/steamos"

    def __init__(self, remote_sum: str):
        self.remote_sum, self.cmds, self.puts = remote_sum, [], []

    def run(self, cmd, **kw):
        self.cmds.append(cmd)
        if cmd.startswith("sha256sum"):
            return 0, f"{self.remote_sum}  x\n" if self.remote_sum else "", ""
        return 0, "", ""

    def put(self, local, remote, **kw):
        self.puts.append(remote)


def test_ensure_helper_uploads_only_when_different(tmp_path, monkeypatch):
    import hashlib

    exe = tmp_path / "fp_venc"
    exe.write_bytes(b"\x7fELF test")
    monkeypatch.setattr(L, "helper_path", lambda: exe)
    same = _Frame(hashlib.sha256(exe.read_bytes()).hexdigest())
    assert L.ensure_helper(same) and not same.puts
    other = _Frame("0" * 64)
    assert L.ensure_helper(other)
    assert len(other.puts) == 1 and other.puts[0].startswith("/home/steamos/.local/share/frameport/bin/fp_venc.new-")
    assert any("chmod 755" in c and "mv -f" in c and other.puts[0] in c for c in other.cmds)
    monkeypatch.setattr(L, "helper_path", lambda: None)
    assert not L.ensure_helper(_Frame(""))  # a build without the helper: software encoder


def test_status_text_names_encoder():
    from frameport.ui.views.live import status_text

    line = status_text({"ready": True, "width": 1280, "height": 720, "fps": 32, "encoder": "hardware",
                        "bytes": 1000, "seconds": 1, "viewers": 1})
    assert "32 fps" in line and "hardware encoder" in line


def test_window_stats_and_panel_warning():
    c, w = L.window_stats("fp_venc: stats frames=320 repeats=5 late=0 skipped=0 kbps=4900", {})
    assert w == {"frames": 320, "dropped": 0, "kbps": 4900, "dropping": False}
    c, w = L.window_stats("fp_venc: stats frames=528 repeats=140 late=172 skipped=104 kbps=6627", c)
    assert w["frames"] == 208 and w["dropped"] == 276 and w["dropping"]  # the owner's 1080p run
    c, w = L.window_stats("fp_venc: stats frames=846 repeats=140 late=176 skipped=104 kbps=6600", c)
    assert w["dropped"] == 4 and not w["dropping"]  # a few late slots in 10 s: no warning
    from frameport.ui.views.live import health_text

    assert "lower quality" in health_text({"dropping": True, "dropped": 276})
    assert health_text({"dropping": False, "dropped": 4}) == "" and health_text({}) == ""


class _StopChan:
    def __init__(self, exits: bool):
        self.exits, self.closed, self.eof = exits, False, False

    def shutdown_write(self):
        self.eof = True

    def exit_status_ready(self):
        return self.exits

    def recv_ready(self):
        return False

    def recv_stderr_ready(self):
        return False

    def close(self):
        self.closed = True


def test_stop_kills_a_stuck_stream():
    """fp_venc blocked in a write never sees stdin EOF: after the grace time its process group is ended."""
    frame = _Frame("")
    src = L.FrameSource(frame, "1080p")
    src.chan, src.info = _StopChan(exits=False), {"pgid": "4321"}
    src.close(grace=0.1)
    assert src.chan.eof and src.chan.closed
    assert any("kill -TERM -- -4321" in c for c in frame.cmds)
    frame = _Frame("")
    src = L.FrameSource(frame, "1080p")
    src.chan, src.info = _StopChan(exits=True), {"pgid": "4321"}
    src.close(grace=0.1)
    assert not any("kill" in c for c in frame.cmds)  # stopped by itself: nothing to kill


def test_status_has_dropped_frames():
    live = L.LiveStream(lambda: _Source(b""))
    try:
        live.source_info = lambda: {"encoder": "hardware", "fps": "32", "stats": {"dropped": 9, "dropping": True}}
        st = live.status()
        assert st["dropped"] == 9 and st["dropping"]
        live.source_info = lambda: {"encoder": "software", "fps": "30", "stats": {}}
        assert "dropping" not in live.status()  # x264 path: no stats
    finally:
        live.server.server_close()
