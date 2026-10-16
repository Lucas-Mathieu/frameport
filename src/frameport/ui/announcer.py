"""The announcement pop-up (frameport.announcements does the fetching and the who-sees-it rules): checked in the
background a little after start and every 6 h, shown once when no other dialog is open and the welcome screen is done.
Any way of closing it (the X, a click outside, Escape, its button) dismisses that announcement for good."""
from __future__ import annotations

import threading
import time
from typing import TYPE_CHECKING

import flet as ft

from .. import announcements
from ..core import applog
from ..i18n import tr
from . import components as C
from . import theme as T

if TYPE_CHECKING:
    from ..announcements import Announcement
    from .app import FramePortApp

FIRST_DELAY = 20   # seconds after start: after the start-up work and the update check
BUSY_RETRY = 15    # another dialog is open / the welcome screen is shown: look again
LONG_BODY = 900    # characters: longer bodies scroll inside the pop-up


class Announcer:
    def __init__(self, app: FramePortApp):
        self.app = app
        self.shown: str | None = None  # id on screen (a check while it's open doesn't show it twice)
        try:
            announcements.since()  # first start → only announcements published after it (before anything is saved)
        except Exception:  # noqa: BLE001
            applog.log.exception("announcements: recording the first start failed")

    def start(self) -> None:
        if not announcements.disabled():
            threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self) -> None:
        time.sleep(FIRST_DELAY)
        while True:
            try:
                a = announcements.fetch()
                if announcements.due(a):
                    self._offer(a)
            except Exception:  # noqa: BLE001
                applog.log.exception("announcement check failed")
            time.sleep(announcements.CHECK_EVERY)

    def _offer(self, a: Announcement) -> None:
        """Wait for a quiet moment (no dialog, not the welcome screen), then show it (background thread)."""
        image = announcements.image_file(a)
        while self.app.route[0] == "welcome" or C.dialog_open(self.app.page):
            time.sleep(BUSY_RETRY)
        if announcements.due(a) and self.shown != a.id:
            self.show(a, image)

    # ---------------------------------------------------------------- the pop-up
    def show(self, a: Announcement, image=None) -> None:
        page = self.app.page
        closed = []

        def dismiss(e=None) -> None:
            if closed:
                return
            closed.append(True)
            self.shown = None
            try:
                announcements.dismiss(a.id)
            except Exception:  # noqa: BLE001
                applog.log.exception("announcement: saving the dismissal failed")

        def close(e=None) -> None:
            dismiss()
            page.pop_dialog()

        def act(e=None) -> None:
            close()
            self.app.open_url(a.url)

        self.shown = a.id
        page.show_dialog(C.dialog(None, announcement_card(page, a, image, close, act if a.button else None),
                                  content_padding=0, actions_padding=0, inset_padding=self._insets(),
                                  clip_behavior=ft.ClipBehavior.ANTI_ALIAS,
                                  on_dismiss=dismiss, width=T.DIALOG_M))


    def _insets(self) -> ft.Padding:
        """Centred over the content area (right of the sidebar, left of an open activity panel), not the whole
        window; where that area is narrower than the pop-up, the panel and then the sidebar are left out."""
        gap = T.S5
        try:
            left, right = self.app.content_insets()
            width = float(self.app.page.width or 0)
        except Exception:  # noqa: BLE001
            return ft.Padding.all(gap)
        need = T.DIALOG_M + 2 * gap
        if width and width - left - right < need:
            right = 0
        if width and width - left - right < need:
            left = 0
        return ft.Padding(left + gap, gap, right + gap, gap)


def announcement_card(page: ft.Page, a: Announcement, image, on_close, on_action=None) -> ft.Control:
    """The pop-up's content: the portal edge, the picture (if any) with the close button over it, title, date, the
    Markdown body and the optional action."""
    from ..artwork.thumbs import asset_url
    from .updater import _notes_style, _open_url

    width = T.DIALOG_M
    close_tip = tr("Dismiss")
    title = ft.Text(a.title, size=T.px(20), weight=ft.FontWeight.W_700, color=T.TEXT, selectable=True)
    date = C.meta(time.strftime("%Y-%m-%d", time.localtime(a.date)))
    body = ft.Markdown(a.body, selectable=True, extension_set=ft.MarkdownExtensionSet.GITHUB_WEB,
                       md_style_sheet=_notes_style(),
                       on_tap_link=lambda e: _open_url(page, e.data))
    if len(a.body) > LONG_BODY:
        body = ft.Column([body], scroll=ft.ScrollMode.AUTO, height=T.px(320))

    head: list[ft.Control] = [ft.Container(height=T.px(3), gradient=C.portal_gradient())]
    if image is not None:
        head.append(ft.Stack([
            ft.Image(src=asset_url(image), width=width, height=_image_height(image, width), fit=ft.BoxFit.COVER,
                     semantics_label=a.image_alt or None),
            ft.Container(
                ft.Icon(ft.Icons.CLOSE_ROUNDED, size=T.px(18), color=T.TEXT),
                width=T.px(34), height=T.px(34), border_radius=T.px(17), alignment=ft.Alignment.CENTER,
                bgcolor=ft.Colors.with_opacity(0.62, T.BG), border=ft.Border.all(1, T.soft(T.TEXT, 0.18)),
                tooltip=close_tip, ink=True, on_click=on_close, right=T.S3, top=T.S3),
        ], width=width))
        title_row: ft.Control = title
    else:
        title_row = ft.Row([ft.Container(title, expand=True, padding=ft.Padding(0, T.px(6), 0, 0)),
                            C.icon_btn(ft.Icons.CLOSE_ROUNDED, close_tip, on_click=on_close)],
                           vertical_alignment=ft.CrossAxisAlignment.START, spacing=T.S2)

    text = [ft.Column([title_row, date], spacing=T.px(2), tight=True)]
    if a.body:
        text.append(body)
    if on_action is not None:
        text.append(ft.Row([C.primary(a.button, icon=ft.Icons.OPEN_IN_NEW_ROUNDED, on_click=on_action)],
                           alignment=ft.MainAxisAlignment.END))
    pad = ft.Padding(T.S5, T.S4 if image is None else T.S5, T.S4 if image is None else T.S5, T.S5)
    return ft.Container(
        ft.Column([*head, ft.Container(ft.Column(text, spacing=T.S4, tight=True), padding=pad)], spacing=0, tight=True),
        width=width)


def _image_height(path, width: float) -> float:
    """The picture's height at the pop-up's width, kept between a banner and a third of a 16:9 screen."""
    try:
        from PIL import Image

        with Image.open(path) as im:
            ratio = im.height / im.width
    except Exception:  # noqa: BLE001
        ratio = 9 / 21
    return max(T.px(140), min(width * ratio, T.px(320)))
