"""Install links ("Install with FrameDrop" buttons, frameport:// links, pasted links): read the manifest, ask the user,
download, add to the library and install on the Frame. The protocol itself is in deeplink.py (no Flet)."""
from __future__ import annotations

from typing import TYPE_CHECKING

import flet as ft

from ... import deeplink, pipeline
from ...core import library
from ...i18n import fmt_size, tr
from .. import components as C
from .. import theme as T

if TYPE_CHECKING:
    from ..app import FramePortApp
    from ..jobs import Job



def kind_label(kind: str | None) -> str:
    return {deeplink.APK: tr("Android app (APK)"), deeplink.LINUX: tr("Linux build"),
            deeplink.EXE: tr("Windows program"), deeplink.OBB: tr("game data (OBB)")}.get(kind or "", tr("other file"))


def show_paste_dialog(app: FramePortApp) -> None:
    """Add games → "Install from link…": paste a FrameDrop/FramePort button link, a manifest or a file URL."""
    field = ft.TextField(label=tr("Link"), hint_text="https://framedropvr.com/install?manifest=…", autofocus=True,
                         width=T.px(560), border_color=T.BORDER)

    def go(e=None):
        text = (field.value or "").strip()
        if not text:
            return
        app.page.pop_dialog()
        open_link(app, text, pasted=True)
    field.on_submit = go
    app.page.show_dialog(ft.AlertDialog(
        title=ft.Text(tr("Install from a link")), bgcolor=T.SURFACE_2,
        content=ft.Column([
            C.body(tr("Paste the address of an \"Install with FrameDrop\" button (right-click it → Copy link), a "
                      "FramePort or FrameDrop manifest (.json) or a direct link to an APK, a Linux build (.zip) or a "
                      "Windows program (.exe).")),
            ft.Row([field, C.help_icon("install_links")]),
        ], tight=True, spacing=T.S3, width=T.px(600)),
        actions=[C.ghost(tr("Cancel"), on_click=lambda e: app.page.pop_dialog()),
                 C.primary(tr("Continue"), ft.Icons.ARROW_FORWARD_ROUNDED, on_click=go)]))


def open_link(app: FramePortApp, text: str, pasted: bool = False) -> None:
    """A link from a web page (framedrop://, frameport://) or pasted: read what it offers in the background, then ask
    before anything is downloaded."""
    try:
        req = deeplink.parse(text)
    except deeplink.LinkError as exc:
        app.toast(tr("Can't use that link: {reason}").format(reason=str(exc)), error=True)
        return

    def run(job: Job):
        job.reporter.stage("Reading the install link")
        m = deeplink.fetch_manifest(req)
        sizes = {f.url: deeplink.head_size(f.url) for f in m.files}
        app.page.run_thread(lambda: _confirm(app, m, sizes, pasted))
        return tr("{name}: waiting for your answer").format(name=m.name)
    app.submit(tr("Install link: {host}").format(host=_host(req.source)), run, None, "task")


def _host(url: str) -> str:
    from urllib.parse import urlsplit

    return (urlsplit(url).hostname or url) if "://" in url else url


def _confirm(app: FramePortApp, m: deeplink.Manifest, sizes: dict, pasted: bool) -> None:
    rows = []
    for f in m.files:
        size = sizes.get(f.url)
        rows.append(C.kv(f.filename, " · ".join(p for p in (
            kind_label(f.kind), fmt_size(size) if size else "",
            tr("checksum checked") if f.sha256 else tr("no checksum")) if p)))
    frame = app.frame_state == "connected"
    notes = [C.callout(tr("Only install software from sites you trust. FramePort downloads it from {host} and "
                          "installs it on your Frame.").format(host=m.host or _host(m.source)), "warn")]
    if not frame:
        notes.append(C.callout(tr("No Frame is connected: the game is added to your library now and installs once "
                                  "the Frame is connected."), "info"))
    if m.main.kind == deeplink.LINUX:
        notes.append(C.body(tr("Linux builds must be made for arm64 (x86_64 builds run through translation, slower)."),
                            T.TEXT_3))
    heading = tr("Install {name}?").format(name=m.name)
    intro = tr("You pasted a link to {name}.") if pasted else tr("A web page asked FramePort to install {name}.")
    C.confirm(app.page, heading, intro.format(name=m.name), tr("Download and install"),
              lambda: download_and_install(app, m),
              extra=ft.Column([*rows, *notes], spacing=T.S2, tight=True))


def download_and_install(app: FramePortApp, m: deeplink.Manifest) -> Job:
    def run(job: Job):
        rep = job.reporter
        path = deeplink.download(m, rep)
        rep.stage("Adding to the library")
        g = pipeline.add_from_link(m, path, rep)
        pkg = g["package"]
        try:
            from ...artwork import thumbs

            thumbs.prewarm(pkg)
        except Exception:  # noqa: BLE001 - artwork is optional
            pass
        library.set_setting("ui.welcome_done", True)
        app.open_game(pkg)
        app.page.run_thread(lambda: app.install(pkg, "frame"))  # asks the usual install questions, then queues it
        return tr("Downloaded {name}").format(name=g.get("title") or m.name)
    return app.submit(tr("Download {name}").format(name=m.name), run, None, "task")
