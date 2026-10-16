"""Save backups of Quest games (GitHub discussion #352): "Back up saves" (a job), "Restore saves…" (pick a backup,
confirm; the Frame keeps the replaced saves for one undo), "Open backups folder", and "Back up all saves" on the Frame
page. The work itself is install/saves.py (shared with the CLI)."""
from __future__ import annotations

import time
from typing import TYPE_CHECKING

import flet as ft

from ...errors import explain
from ...i18n import fmt_size, tr, tr_n
from ...install import saves
from .. import components as C
from .. import theme as T

if TYPE_CHECKING:
    from ..app import FramePortApp
    from ..jobs import Job


def _frame(app: FramePortApp):
    return app._target_for("frame").frame


def when(t: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))


def backup_line(b: saves.Backup) -> str:
    """"12 files · 3.4 MB of saves · version 1.2" (+ large files left out)."""
    parts = [tr_n("{n} file", "{n} files", b.files), tr("{size} of saves").format(size=fmt_size(b.bytes))]
    if b.version:
        parts.append(tr("version {version}").format(version=b.version))
    if b.skipped:
        parts.append(tr_n("{n} large file left out", "{n} large files left out", b.skipped))
    return " · ".join(parts)


def backup_saves(app: FramePortApp, pkg: str) -> Job:
    """Back up the game's saves from the Frame to this PC (a job)."""
    title = app._title(pkg)

    def run(job: Job):
        b = saves.backup(_frame(app), pkg, job.reporter)
        return tr("{title}: saves backed up ({line})").format(title=title, line=backup_line(b))
    return app.submit(tr("Back up saves: {title}").format(title=title), run, pkg, "tool-frame")


def backup_all(app: FramePortApp, packages: list[str]) -> Job | None:
    """One job backing up every installed Quest game's saves (a game that runs or fails is reported, the rest go
    on)."""
    if not packages:
        return None

    def run(job: Job):
        done, failed = 0, []
        for i, pkg in enumerate(packages):
            job.reporter.check_cancel()
            job.reporter.log(tr("Backing up {title}").format(title=app._title(pkg)))
            try:
                saves.backup(_frame(app), pkg, job.reporter)
                done += 1
            except saves.SavesError as exc:
                failed.append(f"{app._title(pkg)}: {explain(exc)}")
                job.reporter.log(failed[-1])
            job.reporter.progress((i + 1) / len(packages))
        if failed:
            return tr("Backed up {done} of {total} games' saves; not: {failed}").format(
                done=done, total=len(packages), failed="; ".join(failed[:3]) + (" …" if len(failed) > 3 else ""))
        return tr_n("Backed up the saves of {n} game", "Backed up the saves of {n} games", done)
    return app.submit(tr("Back up all saves"), run, None, "tool-frame")


def open_folder(pkg: str) -> None:
    from ...core import winhost

    d = saves.backups_dir(pkg)
    d.mkdir(parents=True, exist_ok=True)
    winhost.open_folder(d)


def restore_dialog(app: FramePortApp, pkg: str) -> None:
    """Pick one of the game's backups and restore it on the Frame (after a confirmation); undo the last restore."""
    title = app._title(pkg)
    found = saves.list_backups(pkg)
    by_path = {str(b.path): b for b in found}
    group = ft.RadioGroup(value=str(found[0].path) if found else None, content=ft.Column([
        ft.Row([ft.Radio(value=str(b.path), active_color=T.ACCENT),
                ft.Column([C.body(when(b.created) + (tr(" (newest)") if i == 0 else ""), T.TEXT),
                           C.meta(backup_line(b))], spacing=T.px(2), expand=True)],
               spacing=T.S2)
        for i, b in enumerate(found)], spacing=T.S2, scroll=ft.ScrollMode.AUTO, height=min(T.px(300),
                                                                                         T.px(52) * len(found))))
    undo_row = ft.Row([C.meta(tr("Checking the saves on the Frame…"))], spacing=T.S2,
                      vertical_alignment=ft.CrossAxisAlignment.CENTER)

    def restore(e):
        b = by_path.get(group.value or "")
        if not b:
            return

        def go():
            app.page.pop_dialog()

            def run(job: Job):
                r = saves.restore(_frame(app), pkg, b.path, job.reporter)
                return tr("{title}: saves from {when} restored ({n} files)").format(
                    title=title, when=when(b.created), n=r.get("restored", 0))
            app.submit(tr("Restore saves: {title}").format(title=title), run, pkg, "tool-frame")
        C.confirm(app.page, tr("Replace the saves on the Frame?"),
                  tr("{title}'s saves on the Frame are replaced by the backup from {when}. The Frame keeps a copy of "
                     "the saves it has now, so you can undo this.").format(title=title, when=when(b.created)),
                  tr("Restore"), go, danger=True)

    def undo(e):
        def go():
            app.page.pop_dialog()

            def run(job: Job):
                saves.undo_restore(_frame(app), pkg)
                return tr("{title}: the saves from before the last restore are back").format(title=title)
            app.submit(tr("Undo restore: {title}").format(title=title), run, pkg, "tool-frame")
        C.confirm(app.page, tr("Undo the last restore?"),
                  tr("Puts back the saves {title} had before the last restore. The saves it has now become the copy "
                     "you can undo to.").format(title=title), tr("Undo restore"), go, danger=True)

    def fill():
        try:
            info = saves.info(_frame(app), pkg)
        except Exception as exc:  # noqa: BLE001
            undo_row.controls = [C.meta(tr("Couldn't check the saves on the Frame: {error}").format(
                error=explain(exc)))]
            C.update(undo_row)
            return
        now = tr("On the Frame now: {line}").format(line=" · ".join(
            [tr_n("{n} file", "{n} files", int(info.get("files") or 0)),
             tr("{size} of saves").format(size=fmt_size(int(info.get("bytes") or 0)))]))
        rows: list[ft.Control] = [ft.Container(C.meta(now), expand=True)]
        rb = info.get("rollback")
        if rb:
            rows.append(C.ghost(tr("Undo last restore ({when})").format(when=when(rb.get("time") or 0)),
                                ft.Icons.UNDO_ROUNDED, on_click=undo))
        undo_row.controls = rows
        C.update(undo_row)

    restore_btn = C.primary(tr("Restore"), ft.Icons.RESTORE_ROUNDED, on_click=restore, disabled=not found)
    if found:
        listing: ft.Control = group
    else:
        listing = C.callout(tr("No backups of {title} on this PC yet. Back up its saves first (game menu → Back up "
                               "saves).").format(title=title))
    content = ft.Column([
        C.body(tr("Choose a backup to put back on the Frame. Close the game first."), T.TEXT_2),
        listing,
        C.callout(tr("The game's current saves on the Frame are replaced. FramePort keeps a copy of them on the Frame, "
                     "so you can undo the last restore."), "warn"),
        undo_row,
    ], spacing=T.S3, tight=True)
    app.page.show_dialog(C.dialog(
        tr("Restore saves · {title}").format(title=title), content, size="m",
        actions=[C.ghost(tr("Open backups folder"), ft.Icons.FOLDER_OPEN_ROUNDED, on_click=lambda e: open_folder(pkg)),
                 C.ghost(tr("Cancel"), on_click=lambda e: app.page.pop_dialog()), restore_btn]))
    app.run_bg(fill)
