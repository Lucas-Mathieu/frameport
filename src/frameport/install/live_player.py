# ruff: noqa: E501  (the embedded page's JavaScript)
"""The Live view player page (served by livestream.py). A Python string rather than an .html file: the
PyInstaller fallback bundle only carries data files it is told about."""

PLAYER_HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FramePort Live view</title>
<style>
  :root { --bg: #0e1013; --text: #e8eaed; --muted: #9aa0a6; --accent: #66b3ff; --panel: rgba(20, 22, 26, .82); }
  html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
               font: 14px/1.4 system-ui, -apple-system, "Segoe UI", sans-serif; overflow: hidden; }
  video { position: fixed; inset: 0; width: 100%; height: 100%; object-fit: contain; background: #000; }
  #bar { position: fixed; left: 12px; right: 12px; bottom: 12px; display: flex; gap: 12px; align-items: center;
         padding: 8px 12px; border-radius: 10px; background: var(--panel); transition: opacity .3s; }
  body.idle #bar { opacity: 0; }
  #dot { width: 9px; height: 9px; border-radius: 50%; background: var(--muted); flex: none; }
  #dot.live { background: #3ddc84; } #dot.err { background: #ff6b6b; }
  #msg { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  #info { color: var(--muted); font-variant-numeric: tabular-nums; }
  button { background: transparent; color: var(--text); border: 1px solid #3c4043; border-radius: 6px;
           padding: 4px 10px; font: inherit; cursor: pointer; }
  button:hover { border-color: var(--accent); }
</style>
</head>
<body>
<video id="v" muted autoplay playsinline disablepictureinpicture></video>
<div id="bar">
  <span id="dot"></span><span id="msg">Connecting to the Frame…</span><span id="info"></span>
  <button id="snd" hidden title="Browsers start videos muted: click to hear the Frame. With sound the picture runs about a second behind (the browser buffers audio); muted it's a quarter of a second">Sound on</button>
  <button id="fs" title="Full screen (or double-click the picture)">Full screen</button>
</div>
<script>
"use strict";
const video = document.getElementById("v"), dot = document.getElementById("dot");
const msg = document.getElementById("msg"), info = document.getElementById("info");
const FAR_BEHIND = 2.0, KEEP = 10;  // seconds behind the newest data
// Catch-up per mode: keep `cushion` s behind the newest data, play at `rate` while more than `catchUp` behind.
// With sound, Chrome keeps ~0.6 s of audio ahead and stalls below it, so playback settles ~1 s behind whatever we do
// (measured 2026-10-07 against the Frame: 1.1x catch-up 8 stalls / 30 s, none 1-2, same average lag ~1 s; 50 ms
// fragments no better). So with sound: no speed-up (stalls are audible), only the jump when > FAR_BEHIND.
// Muted: ~0.2-0.3 s behind. (window.FP_TUNE overrides these: tests)
const TUNE = Object.assign({muted: {cushion: 0.3, catchUp: 0.7, rate: 1.1},
                            sound: {cushion: 0.3, catchUp: Infinity, rate: 1.0}}, window.FP_TUNE || {});
let session = 0, frames = 0, lastFrames = 0, size = "";

function say(text, state) { msg.textContent = text; dot.className = state || ""; }
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function status() {
  try { const r = await fetch("status", {cache: "no-store"}); return await r.json(); }
  catch (e) { return null; }
}

async function run() {
  const id = ++session;
  let st = await status();
  while (id === session && (!st || !st.ready)) {
    if (st && st.ended) { say("Stopped: " + st.ended + " — start it again in FramePort.", "err"); return; }
    if (!st) { say("FramePort isn't streaming any more. Start Live view in FramePort.", "err"); return; }
    say("Waiting for the first picture from the Frame…");
    await sleep(500); st = await status();
  }
  const type = 'video/mp4; codecs="' + (st.codec || "avc1.640028") + '"';
  const MS = window.ManagedMediaSource || window.MediaSource;
  if (!MS || !MS.isTypeSupported(type)) { say("This browser can't play H.264 video (" + type + "). Copy this page's address into another browser.", "err"); return; }
  size = st.width ? st.width + "×" + st.height : "";
  snd.hidden = !st.audio;
  const ms = new MS();
  video.disableRemotePlayback = true;
  video.src = URL.createObjectURL(ms);
  await new Promise(r => ms.addEventListener("sourceopen", r, {once: true}));
  const sb = ms.addSourceBuffer(type);
  sb.mode = "segments";
  const pending = [];
  const pump = () => {
    if (sb.updating || !pending.length || ms.readyState !== "open") return;
    try { sb.appendBuffer(pending.shift()); } catch (e) { trim(true); }
  };
  const trim = (force) => {
    if (sb.updating || !sb.buffered.length) return;
    const start = sb.buffered.start(0), cut = video.currentTime - (force ? 1 : KEEP);
    if (cut > start) { try { sb.remove(start, cut); } catch (e) {} }
  };
  sb.addEventListener("updateend", () => {
    if (sb.buffered.length) {
      const end = sb.buffered.end(sb.buffered.length - 1);
      const lag = end - video.currentTime, k = video.muted ? TUNE.muted : TUNE.sound;
      if (lag > FAR_BEHIND || video.currentTime < sb.buffered.start(0)) video.currentTime = Math.max(end - k.cushion, 0);
      else video.playbackRate = lag > k.catchUp ? k.rate : lag < k.cushion ? 1.0 : video.playbackRate;
      if (video.paused) video.play().catch(() => {});
    }
    pump();
  });
  try {
    const resp = await fetch("stream.mp4", {cache: "no-store"});
    if (!resp.ok) throw new Error(await resp.text());
    say("Live from the Frame", "live");
    const reader = resp.body.getReader();
    let n = 0;
    while (id === session) {
      const {value, done} = await reader.read();
      if (done) break;
      pending.push(value); frames++;
      if (++n % 60 === 0) trim(false);
      pump();
    }
  } catch (e) {
    say("Connection lost (" + e.message + "), reconnecting…", "err");
  }
  if (id !== session) return;
  await sleep(1000);
  const again = await status();
  if (again && again.ended) { say("Stopped: " + again.ended + " — start it again in FramePort.", "err"); return; }
  run();
}

setInterval(() => {
  const chunks = frames - lastFrames; lastFrames = frames;
  info.textContent = [size, chunks ? "receiving" : ""].filter(Boolean).join(" · ");
}, 1000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) run(); });
const toggleFs = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen();
document.getElementById("fs").onclick = toggleFs;
const snd = document.getElementById("snd");
let wantSound = false;  // the choice survives reconnects (each one makes a new MediaSource)
snd.onclick = () => {
  wantSound = !wantSound; video.muted = !wantSound;
  snd.textContent = wantSound ? "Mute" : "Sound on";
  if (wantSound) video.play().catch(() => {});
};
video.addEventListener("loadedmetadata", () => { video.muted = !wantSound; });
video.ondblclick = toggleFs;
let idle;
document.addEventListener("mousemove", () => {
  document.body.classList.remove("idle"); clearTimeout(idle);
  idle = setTimeout(() => document.body.classList.add("idle"), 2500);
});
run();
</script>
</body>
</html>
"""
