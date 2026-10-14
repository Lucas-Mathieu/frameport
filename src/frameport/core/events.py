"""Progress/log reporting shared by the pipeline, CLI and UI.

Every long-running step takes a `Reporter`. The CLI prints; the UI subscribes and updates widgets.
"""
from __future__ import annotations

import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field


@dataclass
class Event:
    kind: str  # "log" | "stage" | "progress" | "check" | "done" | "error"
    message: str = ""
    stage: str = ""
    fraction: float | None = None
    data: dict = field(default_factory=dict)
    time: float = field(default_factory=time.time)


class Reporter:
    def __init__(self, sinks: list[Callable[[Event], None]] | None = None):
        self._sinks = list(sinks or [])
        self._lock = threading.Lock()
        self.stage_name = ""
        self._stage_started = time.monotonic()
        self._stage_seconds: dict[str, float] = {}
        self.cancelled = threading.Event()

    def subscribe(self, sink: Callable[[Event], None]) -> None:
        with self._lock:
            self._sinks.append(sink)

    def emit(self, event: Event) -> None:
        with self._lock:
            sinks = list(self._sinks)
        for sink in sinks:
            try:
                sink(event)
            except Exception:  # a broken UI sink must never kill a build
                pass

    def log(self, message: str, **data) -> None:
        self.emit(Event("log", message, self.stage_name, data=data))

    def stage(self, name: str, message: str = "") -> None:
        now = time.monotonic()
        previous = self.stage_name
        elapsed = now - self._stage_started
        if previous:
            self._stage_seconds[previous] = self._stage_seconds.get(previous, 0.0) + elapsed
        self.stage_name, self._stage_started = name, now
        self.emit(Event("stage", message or name, name,
                        data={"previous_stage": previous, "previous_stage_seconds": elapsed} if previous else {}))

    def stage_timings(self) -> dict[str, float]:
        """Completed stages plus the current stage, for install diagnostics and benchmark comparisons."""
        result = dict(self._stage_seconds)
        if self.stage_name:
            result[self.stage_name] = result.get(self.stage_name, 0.0) + time.monotonic() - self._stage_started
        return result

    def progress(self, fraction: float, message: str = "", **data) -> None:
        """`data` may carry `speed` (e.g. "42.1 MB/s · ~3 min left") for transfers."""
        self.emit(Event("progress", message, self.stage_name, fraction=max(0.0, min(1.0, fraction)), data=data))

    def check(self, name: str, ok: bool | None, detail: str = "") -> None:
        self.emit(Event("check", detail, self.stage_name, data={"name": name, "ok": ok}))

    def check_cancel(self) -> None:
        if self.cancelled.is_set():
            raise Cancelled()


class Cancelled(Exception):
    pass


def printing_reporter(verbose: bool = True) -> Reporter:
    def sink(e: Event) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime(e.time))
        if e.kind == "stage":
            print(f"{stamp} == {e.message}", flush=True)
        elif e.kind == "check":
            mark = {True: "PASS", False: "FAIL", None: "WARN"}[e.data.get("ok")]
            print(f"{stamp}    [{mark}] {e.data.get('name')}: {e.message}", flush=True)
        elif e.kind == "progress":
            if verbose and e.message:
                speed = f" · {e.data['speed']}" if e.data.get("speed") else ""
                print(f"{stamp}    {e.fraction:.0%} {e.message}{speed}", flush=True)
        elif verbose or e.kind == "error":
            print(f"{stamp}    {e.message}", flush=True)

    return Reporter([sink])
