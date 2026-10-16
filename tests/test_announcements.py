"""Announcements: the committed file parses, who sees one (never on a first start), and dismissing is for good."""
from pathlib import Path

import pytest

from frameport import announcements as A
from frameport.core import library

ROOT = Path(__file__).resolve().parents[1]

DOC = """---
id: one
title: Hello
date: 2026-10-10T12:00:00Z
---
Some **news**.
"""


def ann(text=DOC):
    a = A.parse(text)
    assert a is not None
    return a


def test_committed_announcement_is_valid():
    """announcements/current.md must parse (an inactive one is checked too): a typo fails here, not on users."""
    A.parse((ROOT / A.PATH).read_text(encoding="utf-8"))


def test_parse_fields():
    a = A.parse("""---
id: frameport-1.3
title: "FramePort 1.3: the Monitor"
date: 2026-11-02
image: announcements/x.png
image_alt: A picture
button: Read more
url: https://frameport.app/changelog/
expires: 2026-11-30
---
Body *here*.""")
    assert a.id == "frameport-1.3" and a.title == "FramePort 1.3: the Monitor" and a.body == "Body *here*."
    assert a.image == A.RAW + "announcements/x.png" and a.image_alt == "A picture"
    assert a.button == "Read more" and a.url == "https://frameport.app/changelog/"
    assert a.date == 1793577600.0  # 2026-11-02 00:00 UTC
    assert a.expires == 1795996800.0 + 86400  # end of 2026-11-30 (inclusive)


@pytest.mark.parametrize("text", ["", "   \n", "---\nactive: false\n---\n"])
def test_nothing_to_show(text):
    assert A.parse(text) is None


@pytest.mark.parametrize("front, error", [
    ("title: x\ndate: 2026-10-10", "id"),
    ("id: bad id\ntitle: x\ndate: 2026-10-10", "id"),
    ("id: a\ndate: 2026-10-10", "title"),
    ("id: a\ntitle: x", "date"),
    ("id: a\ntitle: x\ndate: soon", "date"),
    ("id: a\ntitle: x\ndate: 2026-10-10\nbutton: Go", "button and url"),
    ("id: a\ntitle: x\ndate: 2026-10-10\nimage: http://example.com/x.png", "https"),
    ("id: a\ntitle: x\ndate: 2026-10-10\nimage: ../secret.png", "https"),
    ("id: a\ntitle: x\ndate: 2026-10-10\nwhere: app", "unknown"),  # the site pop-up was dropped
    ("id: a\ntitle: x\ndate: 2026-10-10\ncolour: red", "unknown"),
    ("active: false\nid: a\ntitle: x\ndate: tomorrow", "date"),  # switched off, still checked
])
def test_parse_errors(front, error):
    with pytest.raises(A.AnnouncementError, match=error):
        A.parse(f"---\n{front}\n---\nbody")


def test_fetch_never_raises_on_a_broken_file(monkeypatch):
    monkeypatch.setattr(A.cache, "cached_text", lambda *a, **k: "no front matter")
    assert A.fetch() is None


def test_first_start_sees_no_older_announcement():
    """A new install: `since` = now, so an announcement published before it never shows."""
    assert A.since() > 1.9e9 - 2e8  # roughly now
    assert not A.due(ann())


def test_existing_user_sees_it_once():
    library.set_setting("update.last_seen_version", "1.2.0")  # FramePort ran before announcements existed
    assert A.since() == 0
    a = ann()
    assert A.due(a)
    A.dismiss(a.id)
    assert not A.due(a)
    assert not A.due(a)  # for good, also at the next check
    # a typo fixed under the same id stays dismissed; a new id shows again
    assert not A.due(ann(DOC.replace("Hello", "Hallo")))
    assert A.due(ann(DOC.replace("id: one", "id: two")))


def test_since_is_recorded_once():
    first = A.since()
    library.set_setting("update.last_seen_version", "1.2.0")  # written later by this same start
    assert A.since() == first


def test_new_user_sees_later_announcements():
    start = 1_800_000_000.0
    later = ann(DOC.replace("2026-10-10T12:00:00Z", "2027-01-20T00:00:00Z"))  # after `start`
    assert A.due(later, now=later.date + 60, user_since=start)
    assert not A.due(ann(), now=later.date + 60, user_since=start)  # published before the install


def test_schedule_and_expiry():
    a = ann(DOC.replace("---\nSome", "expires: 2026-10-12\n---\nSome"))
    assert not A.due(a, now=a.date - 60, user_since=0)        # not published yet
    assert A.due(a, now=a.date + 60, user_since=0)
    assert not A.due(a, now=a.expires + 1, user_since=0)      # expired


def test_dismissed_list_is_bounded():
    for i in range(A.KEEP_DISMISSED + 5):
        A.dismiss(f"id{i}")
    kept = A.dismissed()
    assert len(kept) == A.KEEP_DISMISSED and kept[-1] == f"id{A.KEEP_DISMISSED + 4}"
    A.dismiss("id10")  # again: moves to the end, no duplicate
    assert A.dismissed().count("id10") == 1 and A.dismissed()[-1] == "id10"


def test_image_file_checks_the_picture(monkeypatch, tmp_path):
    from PIL import Image

    src = tmp_path / "pic.png"
    Image.new("RGB", (40, 20), "orange").save(src)

    def fake_download(url, dest, **kw):
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(src.read_bytes() if url.endswith(".png") else b"not an image")
        return dest
    monkeypatch.setattr(A.cache, "download", fake_download)
    good = A.image_file(ann(DOC.replace("---\nSome", "image: announcements/a.png\n---\nSome")))
    assert good is not None and good.suffix == ".png" and good.exists()
    assert A.image_file(ann(DOC.replace("---\nSome", "image: https://example.com/a.jpg\n---\nSome"))) is None
    assert not list(good.parent.glob("*.part"))
