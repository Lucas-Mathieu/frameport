#!/usr/bin/env python3
"""Analyse every Quest dump below the given folders once and save the analyses as JSON, so heuristics evaluations
(`eval_heuristics.py --snapshot`) don't re-read gigabytes of APKs each run.

    python scripts/heuristics_snapshot.py <out.json> <dumps folder> [<dumps folder> ...]

An original APK beats an already converted one (OVRPort output, e.g. PATCHED/ copies): converted inputs change what
the heuristics suggest, so they are only kept when no original exists and are marked `converted`. The output holds
local paths: keep it out of git.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from frameport.analysis.detect import ANALYSIS_VERSION, analyze  # noqa: E402
from frameport.sources import quest_dump  # noqa: E402


def _skip(game, roots: list[Path]) -> bool:
    """Folders starting with "_" (known-good backups, test builds, keys) are not dumps."""
    for root in roots:
        try:
            rel = game.apk.relative_to(root)
        except ValueError:
            continue
        return any(part.startswith("_") for part in rel.parts[:-1])
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("roots", type=Path, nargs="+")
    args = ap.parse_args()
    roots = [r.resolve() for r in args.roots]
    old = json.loads(args.out.read_text()) if args.out.is_file() else {}
    snap: dict[str, dict] = {}
    for root in roots:
        for g in quest_dump.scan(root):
            if _skip(g, roots):
                continue
            key = str(g.apk)
            cached = next((v for v in old.values() if v.get("apk") == key
                           and v.get("analysis_version") == ANALYSIS_VERSION
                           and v.get("apk_size") == g.apk.stat().st_size), None)
            t = time.time()
            if cached:
                entry = cached
            else:
                try:
                    data_bytes = g.data_bytes()
                    a = analyze(g.apk, data_bytes=data_bytes)
                except Exception as e:  # noqa: BLE001 - one unreadable dump mustn't stop the snapshot
                    print(f"FAIL {g.apk}: {e}", file=sys.stderr, flush=True)
                    continue
                entry = {"analysis": dataclasses.asdict(a), "data_bytes": data_bytes, "apk": key,
                         "apk_size": g.apk.stat().st_size, "name": g.name, "root": str(root),
                         "converted": bool(a.is_overport_output), "analysis_version": ANALYSIS_VERSION}
            pkg = entry["analysis"]["package"]
            have = snap.get(pkg)
            # originals beat converted copies; otherwise the first root given wins
            if have is None or (have["converted"] and not entry["converted"]):
                snap[pkg] = entry
            print(f"{'cache' if cached else f'{time.time() - t:5.1f}s'} {pkg}"
                  f"{' (converted)' if entry['converted'] else ''}", flush=True)
            args.out.write_text(json.dumps(snap, indent=1, default=str))  # partial results survive a crash
    print(f"{len(snap)} packages ({sum(v['converted'] for v in snap.values())} converted only) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
