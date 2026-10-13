#!/usr/bin/env python3
"""Collect every translatable text into src/frameport/locales/template.json (see src/frameport/i18n.py).

    python scripts/i18n_extract.py          # rewrite the template
    python scripts/i18n_extract.py --check  # exit 1 if the template is out of date (CI / tests)

Sources: tr("…") and tr_n("…", "…", n) calls in src/frameport (literal arguments), the GUI help texts, and every
patch's title and description. A translation file (locales/<language>.json) copies the template and fills in the
values: a string for tr() texts, a list [one, other] for tr_n() texts.
"""
from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "frameport"
TEMPLATE = SRC / "locales" / "template.json"
sys.path.insert(0, str(ROOT / "src"))


def from_code() -> tuple[dict, list[str]]:
    """(texts, problems): tr()/tr_n() literals, and calls whose text isn't a literal (can't be extracted)."""
    texts: dict = {}
    problems = []
    for path in sorted(SRC.rglob("*.py")):
        if path.name == "i18n.py" and path.parent == SRC:
            continue
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in ("tr", "tr_n")):
                continue
            args = node.args[:1] if node.func.id == "tr" else node.args[:2]
            if all(isinstance(a, ast.Constant) and isinstance(a.value, str) for a in args) and args:
                if node.func.id == "tr":
                    texts.setdefault(args[0].value, "")
                else:
                    texts[args[0].value] = ["", ""]
            elif any(isinstance(a, ast.JoinedStr) for a in args):
                problems.append(f"{path.relative_to(ROOT)}:{node.lineno}: f-string inside {node.func.id}() "
                                "(use a template and .format())")
    return texts, problems


def from_data() -> dict:
    """Help texts and patch titles/descriptions (defined as data, translated where they're shown)."""
    from frameport.patches import base
    from frameport.patches.settings import GROUPS, UI
    from frameport.ui.help import HELP

    texts = {dict.__getitem__(HELP, k): "" for k in HELP}
    for p in base.all_patches():
        for t in (p.title, p.description, p.summary):
            if t:
                texts[t] = ""
        for attr in ("CHOICES", "RADIUS_CHOICES"):
            for _, label in getattr(p, attr, ()):
                texts[label] = ""
    for _, title in GROUPS:  # the Game settings dialog
        texts[title] = ""
    for ui in UI.values():
        for t in (ui.get("label"), ui.get("help")):
            if t:
                texts[t] = ""
        if ui["control"][0] == "choice":
            for _, label in ui["control"][1]:
                if any(c.isalpha() for c in label):
                    texts[label] = ""
    return texts


def build() -> tuple[dict, list[str]]:
    code, problems = from_code()
    texts = {**from_data(), **code}
    return dict(sorted(texts.items(), key=lambda kv: kv[0].lower())), problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="only check that the template is up to date")
    args = ap.parse_args()
    texts, problems = build()
    for p in problems:
        print(p, file=sys.stderr)
    text = json.dumps(texts, ensure_ascii=False, indent=1) + "\n"
    if args.check:
        current = TEMPLATE.read_text(encoding="utf-8") if TEMPLATE.exists() else ""
        if current != text:
            print("locales/template.json is out of date: run python scripts/i18n_extract.py", file=sys.stderr)
            return 1
        return 1 if problems else 0
    TEMPLATE.parent.mkdir(parents=True, exist_ok=True)
    TEMPLATE.write_text(text, encoding="utf-8")
    print(f"{len(texts)} texts -> {TEMPLATE.relative_to(ROOT)}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
