"""Announcements: one short message from the maintainer, shown once as a pop-up in the app.

Source of truth: `announcements/current.md` on GitHub `main` (front matter + Markdown, announcements/README.md), read
through raw.githubusercontent like the catalog (cached, every 6 h), so a push reaches users without a release.

Who sees it: only people who were already using FramePort before the announcement's `date` (a first start never
shows one, and nor does one published before FramePort was installed). Closing it in any way (the X, a click outside,
Escape, its button) dismisses that `id` for good; a new announcement = a new id. No Flet here.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import logging
import os
import re
import time
from dataclasses import dataclass
from pathlib import Path

import yaml

from . import REPO_URL
from .core import cache, library
from .core.paths import user_data_dir

_log = logging.getLogger("frameport.announcements")

REPO = REPO_URL.removeprefix("https://github.com/").strip("/")
PATH = "announcements/current.md"
RAW = f"https://raw.githubusercontent.com/{REPO}/main/"
URL = RAW + PATH
CHECK_EVERY = 6 * 3600
KEEP_DISMISSED = 50          # ids remembered (oldest dropped)
IMAGE_MAX = 5 * 2**20
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")
# settings only an earlier GUI run writes: one of them when announcements first run = an existing user (not
# recipes.app_version: library.load() writes that on the very first load)
USED_BEFORE = ("update.last_seen_version", "update.last_check", "ui.welcome_done", "ui.window")


@dataclass(frozen=True)
class Announcement:
    id: str
    title: str
    body: str                  # Markdown
    date: float                # unix time it was published
    image: str | None = None   # https URL (repo paths are resolved to raw GitHub)
    image_alt: str = ""
    button: str | None = None  # optional action: label + url
    url: str | None = None
    expires: float | None = None


class AnnouncementError(ValueError):
    pass


def _when(value, end_of_day: bool = False) -> float:
    """A front-matter date (YAML date/datetime or ISO text) as unix time; a bare date is UTC midnight (or the end of
    that day for `expires`). Naive times are UTC."""
    if isinstance(value, str):
        text = value.strip().replace("Z", "+00:00")
        try:
            value = dt.datetime.fromisoformat(text) if "T" in text or " " in text else dt.date.fromisoformat(text)
        except ValueError as exc:
            raise AnnouncementError(f"not a date: {value!r}") from exc
    if isinstance(value, dt.datetime):
        return (value if value.tzinfo else value.replace(tzinfo=dt.timezone.utc)).timestamp()
    if isinstance(value, dt.date):
        day = dt.datetime(value.year, value.month, value.day, tzinfo=dt.timezone.utc)
        return (day + dt.timedelta(days=1 if end_of_day else 0)).timestamp()
    raise AnnouncementError(f"not a date: {value!r}")


def _link(value) -> str | None:
    """An image/button target: https URLs as they are, a repo path (announcements/x.png) → its raw GitHub URL."""
    if value in (None, ""):
        return None
    text = str(value).strip()
    if text.startswith("https://"):
        return text
    if "://" in text or text.startswith(("/", "..")) or ".." in text.split("/"):
        raise AnnouncementError(f"use an https URL or a path in the repo: {text!r}")
    return RAW + text


def parse(text: str | None) -> Announcement | None:
    """The file's announcement; None when there is none (empty file, `active: false`). Raises AnnouncementError for a
    malformed one (the app then shows nothing; tests/test_announcements.py checks the committed file)."""
    if not text or not text.strip():
        return None
    m = re.match(r"\A﻿?---\s*\n(.*?)\n---\s*(?:\n|\Z)(.*)\Z", text, re.S)
    if not m:
        raise AnnouncementError("missing the --- front matter")
    try:
        meta = yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError as exc:
        raise AnnouncementError(f"front matter: {exc}") from exc
    if not isinstance(meta, dict):
        raise AnnouncementError("front matter must be key: value lines")
    active = meta.get("active", True)
    if active is False and not meta.get("id"):
        return None
    if not isinstance(active, bool):
        raise AnnouncementError("active: true or false")
    unknown = set(meta) - {"id", "title", "date", "image", "image_alt", "button", "url", "expires", "active"}
    if unknown:
        raise AnnouncementError(f"unknown fields: {', '.join(sorted(unknown))}")
    ident, title = str(meta.get("id") or "").strip(), str(meta.get("title") or "").strip()
    if not ID_RE.match(ident):
        raise AnnouncementError("id: letters, digits, '.', '_' or '-' (change it to show a new announcement)")
    if not title:
        raise AnnouncementError("title is required")
    if "date" not in meta:
        raise AnnouncementError("date is required (only people using FramePort before it see the announcement)")
    button, url = meta.get("button"), meta.get("url")
    if bool(button) != bool(url):
        raise AnnouncementError("button and url go together")
    if not active:  # switched off, but still checked (tests catch mistakes before it's switched on)
        _check_dates(meta)
        return None
    return Announcement(id=ident, title=title, body=m.group(2).strip(), date=_when(meta["date"]),
                        image=_link(meta.get("image")), image_alt=str(meta.get("image_alt") or ""),
                        button=str(button).strip() if button else None, url=_link(url),
                        expires=_when(meta["expires"], end_of_day=True) if meta.get("expires") else None)


def _check_dates(meta: dict) -> None:
    _when(meta["date"])
    if meta.get("expires"):
        _when(meta["expires"], end_of_day=True)
    _link(meta.get("image"))
    _link(meta.get("url"))


def disabled() -> bool:
    return bool(os.environ.get("FRAMEPORT_NO_ANNOUNCEMENTS"))


def fetch(force: bool = False) -> Announcement | None:
    """The current announcement (cached 6 h; offline → the last copy; never raises)."""
    try:
        return parse(cache.cached_text("announcement.md", URL, max_age=0 if force else CHECK_EVERY, fallback=""))
    except Exception as exc:  # noqa: BLE001
        _log.warning("announcement skipped: %s", exc)
        return None


# ------------------------------------------------------------------------------------------ who sees it
def since() -> float:
    """When this user started using FramePort, as far as announcements are concerned (setting announce.since). Set
    once, the first time it's asked: 0 for someone who used FramePort before announcements existed, else now (a first
    start). Call it early at start, before anything records this start in the settings."""
    value = library.setting("announce.since")
    if isinstance(value, (int, float)):
        return float(value)
    value = 0.0 if any(library.setting(k) is not None for k in USED_BEFORE) or library.games() else time.time()
    library.set_setting("announce.since", value)
    return value


def dismissed() -> list[str]:
    value = library.setting("announce.dismissed")
    return [v for v in value if isinstance(v, str)] if isinstance(value, list) else []


def dismiss(ident: str) -> None:
    """Never show this announcement again (any way of closing it)."""
    library.update_setting("announce.dismissed",
                           lambda cur: ([v for v in (cur or []) if v != ident] + [ident])[-KEEP_DISMISSED:], [])


def due(a: Announcement | None, now: float | None = None, user_since: float | None = None) -> bool:
    """Whether to show `a` now: published, not expired, not dismissed, and published after this
    user started using FramePort."""
    if a is None:
        return False
    now = time.time() if now is None else now
    if a.expires is not None and now >= a.expires:
        return False
    if a.date > now:  # scheduled for later
        return False
    if a.id in dismissed():
        return False
    return a.date > (since() if user_since is None else user_since)


def image_file(a: Announcement) -> Path | None:
    """The announcement's image, downloaded once into <data>/announcements/ and checked (Pillow); None without one or
    when it can't be loaded (the pop-up then shows no image). Shown by asset URL like artwork, never as bytes."""
    if not a.image:
        return None
    folder = user_data_dir() / "announcements"
    stem = hashlib.sha256(a.image.encode()).hexdigest()[:16]
    for done in folder.glob(f"{stem}.*"):
        if done.suffix != ".part":
            return done
    try:
        from PIL import Image

        tmp = folder / f"{stem}.part"
        cache.download(a.image, tmp)
        if tmp.stat().st_size > IMAGE_MAX:
            raise AnnouncementError("image larger than 5 MiB")
        with Image.open(tmp) as im:
            im.verify()
            fmt = (im.format or "").lower()
        ext = {"jpeg": ".jpg", "png": ".png", "webp": ".webp", "gif": ".gif"}.get(fmt)
        if not ext:
            raise AnnouncementError(f"image format {fmt or '?'} (use PNG, JPEG, WebP or GIF)")
        out = folder / f"{stem}{ext}"
        tmp.replace(out)
        return out
    except Exception as exc:  # noqa: BLE001
        _log.warning("announcement image skipped: %s", exc)
        (folder / f"{stem}.part").unlink(missing_ok=True)
        return None
