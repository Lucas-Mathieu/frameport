"""A complete Steam library art set for a shortcut, composed from whatever artwork a game has.

Steam wants: portrait 600×900 (library grid), landscape 920×430 (recent/horizontal), hero 1920×620 (game page banner),
logo (transparent, drawn on the hero) and an icon. Store art rarely covers all of them — Rift games from OculusDB only
have a square cover — so missing shapes are composed: the best source image, fitted and centred over a blurred,
darkened copy of itself (or a screenshot, for the hero). Results are cached in artwork/<pkg>/steam/.
A game without any artwork (e.g. an Android app that isn't on a VR store) gets a placeholder set: its name on a
coloured background, with the APK's own launcher icon when it has a bitmap one.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

from . import fetch

SIZES = {"portrait": (600, 900), "landscape": (920, 430), "hero": (1920, 620)}
# preferred sources per Steam shape, best first ("shot" = first screenshot)
PREFER = {"portrait": ("portrait", "square", "icon", "landscape"),
          "landscape": ("landscape", "hero", "shot", "square", "portrait"),
          "hero": ("hero", "shot", "landscape", "square", "portrait"),
          "icon": ("icon", "square", "portrait")}


def _sources(package: str) -> dict[str, Path]:
    files = {p.stem: p for p in fetch.files(package)}
    shots = sorted(fetch.artwork_dir(package).glob("shot_*.jpg"))
    if shots:
        files["shot"] = shots[0]
    return files


def _fill(im, size):
    """Scale to cover `size` and centre-crop."""
    from PIL import Image

    w, h = size
    s = max(w / im.width, h / im.height)
    im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)
    left, top = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((left, top, left + w, top + h))


def _compose(src: Path, size, exact: bool):
    """exact: the source already has (about) the right shape → cover-fit; otherwise fit over a blurred backdrop."""
    from PIL import Image, ImageEnhance, ImageFilter

    with Image.open(src) as im:
        im = im.convert("RGB")
        w, h = size
        if exact:
            return _fill(im, size)
        bg = _fill(im, size).filter(ImageFilter.GaussianBlur(radius=max(w, h) // 30))
        bg = ImageEnhance.Brightness(bg).enhance(0.45)
        s = min(w / im.width, h / im.height) * 0.94
        fg = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)
        bg.paste(fg, ((w - fg.width) // 2, (h - fg.height) // 2))
        return bg


def _ratio_ok(src: Path, size) -> bool:
    from PIL import Image

    with Image.open(src) as im:
        r = im.width / im.height
    target = size[0] / size[1]
    return abs(r - target) / target < 0.18


def steam_set(package: str, title: str = "", apk: str | Path | None = None) -> dict[str, Path]:
    """{portrait, landscape, hero, logo?, icon?} files ready for Steam (created once per source change). Without any
    artwork: a placeholder set showing `title` (+ the launcher icon from `apk`), or {} without a title."""
    src = _sources(package)
    if not src and not title:
        return {}
    title = title or package
    out_dir = fetch.artwork_dir(package) / "steam"
    out_dir.mkdir(parents=True, exist_ok=True)
    # no store art (at most the APK's icon; the GUI's generated cover/banner don't count): the name on a coloured tile
    placeholder = not (set(src) & (STORE_KINDS | {"shot"}))
    parts = [f"{k}:{p.stat().st_size}:{p.stat().st_mtime_ns}" for k, p in sorted(src.items())]
    if placeholder:
        parts.append(f"placeholder:{PLACEHOLDER_VERSION}:{title}:{apk or ''}")
    key = hashlib.sha1("|".join(parts).encode()).hexdigest()[:8]
    stamp = out_dir / f".{key}"
    result: dict[str, Path] = {}
    if stamp.exists():
        for p in out_dir.iterdir():
            if not p.name.startswith("."):
                result[p.stem] = p
        return result
    for p in out_dir.iterdir():
        p.unlink()
    if placeholder:
        result = _placeholder(out_dir, title, src.get("icon"), apk)
        stamp.write_text("")
        return result
    for kind, size in SIZES.items():
        choice = next((k for k in PREFER[kind] if k in src), None)
        if not choice:
            continue
        try:
            # a screenshot always fills the wide banner; other sources only when their shape is close enough
            exact = _ratio_ok(src[choice], size) or (kind == "hero" and choice == "shot")
            img = _compose(src[choice], size, exact)
            path = out_dir / f"{kind}.jpg"
            img.save(path, "JPEG", quality=90, optimize=True)
            result[kind] = path
        except Exception:  # noqa: BLE001 - unreadable source: skip that shape
            continue
    if "logo" in src:
        result["logo"] = out_dir / f"logo{src['logo'].suffix}"
        result["logo"].write_bytes(src["logo"].read_bytes())
    icon = next((src[k] for k in PREFER["icon"] if k in src), None)
    if icon:
        try:
            from PIL import Image

            with Image.open(icon) as im:
                im = _fill(im.convert("RGBA"), (256, 256))
                path = out_dir / "icon.png"
                im.save(path, "PNG")
                result["icon"] = path
        except Exception:  # noqa: BLE001
            pass
    stamp.write_text("")
    return result


PLACEHOLDER_VERSION = 2


STORE_KINDS = {"portrait", "square", "landscape", "hero"}


def ensure_cover(package: str) -> Path | None:
    """A cover for FramePort's own library when a game has no store art (e.g. a 2D Android app): the same name +
    APK icon design as the Steam placeholder, saved as artwork/<pkg>/cover.jpg. None when store art exists."""
    from ..core import library

    d = fetch.artwork_dir(package)
    if {p.stem for p in fetch.files(package)} & STORE_KINDS:
        return None
    g = library.game(package) or {}
    if not any(d.glob("icon.*")):  # the icon also shows in lists (Frame page)
        icon = fetch.apk_icon(g.get("apk"))
        if icon:
            (d / "icon.png").write_bytes(icon)
    # icon-only (the card and the game page show the name anyway): a tall cover for cards, a wide banner for the page
    title = g.get("title") or package
    cover, banner = d / "cover.jpg", d / "banner.jpg"
    stamp = d / ".cover"
    key = f"{PLACEHOLDER_VERSION}:{title}:{next((p.stat().st_size for p in d.glob('icon.*')), 0)}"
    if cover.exists() and banner.exists() and stamp.exists() and stamp.read_text(encoding="utf-8") == key:
        return cover
    icon = _load_icon(next(iter(sorted(d.glob("icon.*"))), None), g.get("apk"))
    background = _background_for(title)
    # cover: icon a bit above the middle (the card's title sits at the bottom); banner: icon on the right (the game
    # page draws the title on the left)
    for path, size, side, centre in ((cover, (600, 900), 300, (0.5, 0.4)), (banner, (1600, 600), 320, (0.75, 0.5))):
        im = background(size)
        if icon is not None:
            ic = icon.resize((side, side))
            im.paste(ic, (round(size[0] * centre[0] - side / 2), round(size[1] * centre[1] - side / 2)), ic)
        im.save(path, "JPEG", quality=90, optimize=True)
    stamp.write_text(key, encoding="utf-8")
    return cover


def steam_set_for(package: str) -> dict[str, Path]:
    """steam_set() with the library entry's title and APK, so a game without artwork gets a placeholder."""
    from ..core import library

    g = library.game(package) or {}
    return steam_set(package, title=g.get("title") or package, apk=g.get("apk"))


def _apk_icon(apk: str | Path | None):
    """The APK's launcher icon as a PIL image, or None (see fetch.apk_icon)."""
    import io

    from PIL import Image

    data = fetch.apk_icon(apk)
    if not data:
        return None
    with Image.open(io.BytesIO(data)) as im:
        return im.convert("RGBA")


def _font(size: int):
    from PIL import ImageFont

    try:
        return ImageFont.load_default(size=size)  # Pillow's bundled scalable font (needs FreeType)
    except Exception:  # noqa: BLE001
        return ImageFont.load_default()


def _wrap(draw, text: str, font, width: int) -> list[str]:
    lines: list[str] = []
    for word in text.split():
        if lines and draw.textlength(f"{lines[-1]} {word}", font=font) <= width:
            lines[-1] += f" {word}"
        else:
            lines.append(word)
    return lines or [text]


def _title_block(draw, text: str, box: tuple[int, int, int, int], max_size: int, fill) -> None:
    """Draw `text` centred in box (x0, y0, x1, y1), as large as fits (wrapped, at most 3 lines)."""
    x0, y0, x1, y1 = box
    size = max_size
    while True:
        font = _font(size)
        lines = _wrap(draw, text, font, x1 - x0)
        line_h = round(size * 1.2)
        too_wide = any(draw.textlength(line, font=font) > x1 - x0 for line in lines)
        if size <= 14 or (len(lines) <= 3 and not too_wide and line_h * len(lines) <= y1 - y0):
            break
        size = round(size * 0.9)
    top = y0 + (y1 - y0 - line_h * len(lines)) // 2
    for i, line in enumerate(lines):
        w = draw.textlength(line, font=font)
        draw.text((x0 + (x1 - x0 - w) / 2, top + i * line_h), line, font=font, fill=fill)


def _background_for(title: str):
    """A vertical gradient in a colour picked from the title: background(size) -> PIL image."""
    import colorsys

    from PIL import Image

    hue = int(hashlib.sha1(title.encode()).hexdigest()[:4], 16) / 0xFFFF
    top = tuple(round(c * 255) for c in colorsys.hls_to_rgb(hue, 0.30, 0.45))
    bottom = tuple(round(c * 255) for c in colorsys.hls_to_rgb(hue, 0.10, 0.40))

    def background(size):
        mask = Image.linear_gradient("L").resize(size)  # 0 at the top → 255 at the bottom
        return Image.composite(Image.new("RGB", size, bottom), Image.new("RGB", size, top), mask)
    return background


def _load_icon(icon_file: Path | None, apk):
    from PIL import Image

    if icon_file:
        try:
            with Image.open(icon_file) as im:
                return im.convert("RGBA")
        except Exception:  # noqa: BLE001
            pass
    return _apk_icon(apk)


def _placeholder(out_dir: Path, title: str, icon_file: Path | None, apk) -> dict[str, Path]:
    """Name-on-colour art (portrait, landscape, hero, logo, icon), the hue picked from the title."""
    from PIL import Image, ImageDraw

    icon = _load_icon(icon_file, apk)
    background = _background_for(title)

    result: dict[str, Path] = {}
    for kind, (w, h) in SIZES.items():
        im = background((w, h))
        draw = ImageDraw.Draw(im)
        if kind != "hero":  # Steam draws the logo (the name) over the hero
            text_box = (w // 10, h // 10, w - w // 10, h - h // 10)
            if icon is not None:
                side = min(w, h) // 3
                ic = icon.resize((side, side), Image.LANCZOS)
                if kind == "portrait":
                    im.paste(ic, ((w - side) // 2, h // 4 - side // 4), ic)
                    text_box = (w // 10, h // 4 + side, w - w // 10, h - h // 10)
                else:
                    im.paste(ic, (w // 10, (h - side) // 2), ic)
                    text_box = (w // 10 + side + w // 20, h // 10, w - w // 12, h - h // 10)
            _title_block(draw, title, text_box, h // 7 if kind == "portrait" else h // 5, (255, 255, 255))
        path = out_dir / f"{kind}.jpg"
        im.save(path, "JPEG", quality=90, optimize=True)
        result[kind] = path
    logo = Image.new("RGBA", (1200, 400), (0, 0, 0, 0))
    _title_block(ImageDraw.Draw(logo), title, (20, 20, 1180, 380), 150, (255, 255, 255, 255))
    logo = logo.crop(logo.getbbox() or (0, 0, 1200, 400))
    result["logo"] = out_dir / "logo.png"
    logo.save(result["logo"], "PNG")
    ic = background((256, 256))
    if icon is not None:
        inner = icon.resize((216, 216), Image.LANCZOS)
        ic.paste(inner, (20, 20), inner)
    else:
        _title_block(ImageDraw.Draw(ic), title[:2].upper() if len(title) > 3 else title, (24, 24, 232, 232), 140,
                     (255, 255, 255))
    result["icon"] = out_dir / "icon.png"
    ic.save(result["icon"], "PNG")
    return result


def original_platform(entry: dict) -> str:
    """What the game was made for (a Steam tag)."""
    extra = (entry.get("analysis") or {}).get("extra") or {}
    if entry.get("kind") == "linux":
        return "Linux"
    if entry.get("kind") == "rift":
        if extra.get("flat"):
            return "Windows"
        return "Oculus Rift" if extra.get("needs_revive") else "PC VR"
    kind = extra.get("vr_kind") or "quest"
    if kind == "quest" and extra.get("headset") in ("gearvr", "go"):
        return {"gearvr": "Gear VR", "go": "Oculus Go"}[extra["headset"]]
    return {"quest": "Meta Quest", "none": "Android"}.get(kind, "Android VR")


def steam_tags(entry: dict, where: str = "frame") -> list[str]:
    """Tags for the Steam shortcut (Steam can group them into collections): how it runs, the game's original platform,
    genres and the user's own tags."""
    from ..targets.pc_revive import TAG

    kind = ((entry.get("analysis") or {}).get("extra") or {}).get("vr_kind") or "quest"
    flat = bool(((entry.get("analysis") or {}).get("extra") or {}).get("flat"))
    runs = TAG if where != "frame" else ("Windows game on Frame" if flat else "PC VR on Frame") \
        if entry.get("kind") == "rift" else "Linux app on Frame" if entry.get("kind") == "linux" else \
        "Quest on Frame" if kind == "quest" else "Android on Frame"
    base = [runs, original_platform(entry)]
    genres = ((entry.get("details") or {}).get("genres") or [])[:4]
    return list(dict.fromkeys(base + genres + list(entry.get("tags") or [])))
