#!/usr/bin/env python3
"""Evaluate static patch suggestions against catalog-only ground truth (no catalog-assisted suggestions).

    eval_heuristics.py <dumps folder> [--verbose]
    eval_heuristics.py --snapshot analyses.json --split all --json baseline.json
    eval_heuristics.py --snapshot analyses.json --fit-report --out fit-report.md

Snapshots can be in progress: each read copies to a temporary file and retries incomplete JSON. The split is
stratified by analysis engine and stable for a fixed set of packages. Converted inputs are included and labelled.
Patch scores are micro-averaged across games, excluding enabled automatic defaults. Expected defaults are detected
per analysis before applying explicit catalog removals; inapplicable defaults create no removal decisions. A wrong
adapter value is both an FP and an FN. Removal decisions and alternate-build patches have their own identities
("remove:<id>", "alt:<id>").
Unsupported catalog entries and runtime-only choices are reported separately from the headline.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from frameport.analysis.detect import analyze  # noqa: E402
from frameport.core.library import analysis_from_dict  # noqa: E402
from frameport.core.models import Recipe  # noqa: E402
from frameport.patches import base  # noqa: E402
from frameport.recommend import catalog, engine  # noqa: E402
from frameport.sources import quest_dump  # noqa: E402

RUNTIME_ONLY = [
    "use_alt", "adapter.vk_shader_fix", "adapter.focus_hold", "adapter.focus_hold_ms", "adapter.sync_guard",
    "adapter.scale", "adapter.layer_debug", "adapter.refresh_rate", "adapter.pose_consistency",
    "lepton_env", "config_sync",
]
REPO = Path(__file__).resolve().parents[1]
DEVICE_FIELDS = {"device.files", "device.lepton_env", "device.config_sync", "device.foveation"}


def expected_defaults(analysis) -> set[str]:
    """Defaults recommended by their own detection, before recipe normalization/removals.

    As in suggest's initial pass, default_on patches bypass the opt-in applies gate; their own detect() decides.
    This masks e.g. LTW defaults in games without LTW, without importing any heuristic opt-in/adapter choices.
    """
    return {p.id for p in base.all_patches() if p.default_on and base.for_game(p, analysis)
            and (s := p.detect(analysis)) and s.recommended}


def signature(recipe: Recipe, defaults: set[str] | None = None) -> dict:
    """Recipe decisions, excluding enabled defaults but retaining explicit removal of defaults."""
    pats = recipe.patches
    rift = recipe.package.startswith("rift.")
    patches = [p for p in base.all_patches() if (p.category == "pcvr") == rift]
    out = {}
    for category in ("overport", "frame", "pcvr"):
        out[category] = sorted(p.id for p in patches if p.category == category and not p.default_on and p.id in pats)
        out[category + "_removed"] = sorted(p.id for p in patches
                                            if p.category == category and p.default_on and p.id not in pats
                                            and (defaults is None or p.id in defaults))
    out.update({
        "adapter": {p.split(".", 1)[1]: v.get("value") for p, v in sorted(pats.items()) if p.startswith("adapter.")},
        "device": sorted(p for p in pats if p.startswith("device.") and p not in DEVICE_FIELDS
                         and not base.get(p).default_on),
        "device_files": sorted((pats.get("device.files") or {}).get("files", {})),
        "foveation": dict(pats.get("device.foveation") or {}),
        "lepton_env": dict((pats.get("device.lepton_env") or {}).get("env", {})),
        "config_sync": dict((pats.get("device.config_sync") or {}).get("files", {})),
        "alt": sorted(recipe.alt_patches),
        "use_alt": recipe.use_alt,
        "status_unsupported": recipe.status == "unsupported",
    })
    return out


def catalog_recipe(entry: catalog.CatalogEntry, defaults: set[str] | None = None, analysis=None) -> Recipe:
    """Materialize catalog choices plus expected defaults; never import heuristic opt-in choices. With the game's
    analysis, the engine's structural transforms apply as in suggest(): a 32-bit game goes through the bridge (its
    entry relies on that instead of listing what the bridge replaces), a 2D app keeps only its non-VR patches."""
    r = Recipe(entry.package, status=entry.status, as_is=entry.as_is,
               alt_patches=list(entry.alt_overport), use_alt=entry.use_alt)
    rift = entry.kind == "rift" or entry.package.startswith("rift.")
    r.patches = {p.id: {} for p in base.all_patches() if p.default_on and (p.category == "pcvr") == rift
                 and (defaults is None or p.id in defaults)}
    for pid in (*entry.overport_extra, *entry.frame, *entry.device, *entry.pcvr):
        r.patches[pid] = {}
    for pid in (*entry.overport_remove, *entry.frame_remove, *entry.pcvr_remove):
        r.patches.pop(pid, None)
    for key, value in entry.adapter.items():
        r.patches[f"adapter.{key}"] = {"value": value}
    for pid, param, values in (("device.files", "files", entry.device_files),
                               ("device.lepton_env", "env", entry.lepton_env),
                               ("device.config_sync", "files", entry.config_sync)):
        if values:
            r.patches[pid] = {param: copy.deepcopy(values)}
    if entry.foveation:
        r.patches["device.foveation"] = {"mode": entry.foveation}
        if entry.foveation_radius:
            r.patches["device.foveation"]["radius"] = entry.foveation_radius
        if entry.foveation_test:
            r.patches["device.foveation"]["test"] = True
    if analysis is not None:
        if analysis.only_32bit and "frame.a32_bridge" not in entry.frame_remove:
            r.patches.setdefault("frame.a32_bridge", {})
            engine.a32_recipe(analysis, r, newly=False)
        elif analysis.vr_kind == "none":
            needed = {pid for pid in r.patches if not base.get(pid).needs_vr and pid != "frame.launcher"}
            needed |= {"frame.launcher"} if analysis.has_info_category else set()
            r.patches = {pid: v for pid, v in r.patches.items() if pid in needed}
            r.alt_patches = []
    return r


def catalog_signature(entry: catalog.CatalogEntry, defaults: set[str] | None = None, analysis=None) -> dict:
    return signature(catalog_recipe(entry, defaults, analysis), defaults)


def engine_group(name: str) -> str:
    return name if name in ("Unity", "Unreal") else "Other"


def split_packages(engines: dict[str, str]) -> dict[str, str]:
    """package -> dev/holdout; nearest 20% per engine (half rounded up), lowest sha256 first.

    Only pass the intersection of catalog and analyses. This is stable under input reordering; adding packages can
    change membership at a stratum's cutoff. Tiny strata may have no holdout. Unsupported entries participate too.
    """
    groups = defaultdict(list)
    for pkg, name in engines.items():
        groups[engine_group(name)].append(pkg)
    result = {}
    for packages in groups.values():
        ordered = sorted(packages, key=lambda p: (hashlib.sha256(p.encode()).digest(), p))
        n = (len(ordered) + 2) // 5
        result.update((p, "holdout" if i < n else "dev") for i, p in enumerate(ordered))
    return dict(sorted(result.items()))


def _parts(sig: dict) -> tuple[dict, dict]:
    headline, runtime = {}, {}
    for key, value in sig.items():
        if key == "adapter":
            headline[key] = {k: v for k, v in value.items() if f"adapter.{k}" not in RUNTIME_ONLY}
            runtime[key] = {k: v for k, v in value.items() if f"adapter.{k}" in RUNTIME_ONLY}
        else:
            (runtime if key in RUNTIME_ONLY else headline)[key] = value
    return headline, runtime


def _decisions(sig: dict) -> dict:
    """Patch/action identities -> values. Status is a field metric, not a patch."""
    out = {}
    for field, value in sig.items():
        if field == "status_unsupported":
            continue
        if field in ("adapter", "lepton_env", "config_sync"):
            prefix = "adapter" if field == "adapter" else f"device.{field}"
            out.update((f"{prefix}.{k}", v) for k, v in value.items())
        elif field == "foveation":
            if value:
                out["device.foveation"] = value
        elif field == "use_alt":
            if value:
                out["use_alt"] = value
        else:
            prefix = ("remove:" if field.endswith("_removed") else "alt:" if field == "alt"
                      else "device.files:" if field == "device_files" else "")
            out.update((prefix + p, True) for p in value)
    return out


def _rates(tp: int, fp: int, fn: int) -> dict:
    # No positive decisions yields 0, rather than inflating scores for empty recipes/strata.
    return {"tp": tp, "fp": fp, "fn": fn, "precision": tp / (tp + fp) if tp + fp else 0.0,
            "recall": tp / (tp + fn) if tp + fn else 0.0,
            "f1": 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else 0.0}


def score_signatures(want: dict, got: dict) -> dict:
    """Compare one pair of signatures; value mismatches count as both missing and extra."""
    result = {}
    for scope, w, g in zip(("headline", "runtime"), _parts(want), _parts(got), strict=True):
        wanted, actual = _decisions(w), _decisions(g)
        missing = {k: v for k, v in wanted.items() if k not in actual or actual[k] != v}
        extra = {k: v for k, v in actual.items() if k not in wanted or wanted[k] != v}
        per_patch = {k: _rates(int(k in wanted and k in actual and wanted[k] == actual[k]),
                               int(k in extra), int(k in missing)) for k in sorted(wanted.keys() | actual.keys())}
        fields = {k: w.get(k) == g.get(k) for k in sorted(w.keys() | g.keys())}
        result[scope] = {"exact": all(fields.values()), "fields": fields, "missing": missing, "extra": extra,
                         "per_patch": per_patch}
    return result


def _aggregate(rows: list[dict], scope: str) -> dict:
    fields = defaultdict(lambda: {"matched": 0, "total": 0})
    counts = defaultdict(Counter)
    for row in rows:
        s = row["score"][scope]
        for key, match in s["fields"].items():
            fields[key]["matched"] += int(match)
            fields[key]["total"] += 1
        for key, stats in s["per_patch"].items():
            counts[key].update({k: stats[k] for k in ("tp", "fp", "fn")})
    per_patch = {k: _rates(**v) for k, v in sorted(counts.items())}
    total = Counter()
    for stats in counts.values():
        total.update(stats)
    exact = sum(row["score"][scope]["exact"] for row in rows)
    return {"games": len(rows), "exact": exact, "exact_ratio": exact / len(rows) if rows else 0.0,
            "fields": dict(sorted(fields.items())), "per_patch": per_patch,
            "patches": _rates(*(total[k] for k in ("tp", "fp", "fn")))}


def score_records(records: list[dict]) -> dict:
    """Score {package, engine, status, converted, want, got, ...} records; no filesystem needed."""
    rows = [{**r, "score": score_signatures(r["want"], r["got"])} for r in sorted(records, key=lambda r: r["package"])]

    def metrics(group):
        supported = [r for r in group if r["status"] != "unsupported"]
        unsupported = [r for r in group if r["status"] == "unsupported"]
        return {"total_games": len(group), "converted_games": sum(bool(r.get("converted")) for r in group),
                "excluded_unsupported": len(unsupported),
                "headline": _aggregate(supported, "headline"), "runtime": _aggregate(supported, "runtime"),
                "unsupported": {scope: _aggregate(unsupported, scope) for scope in ("headline", "runtime")}}

    return {**metrics(rows), "by_engine": {name: metrics([r for r in rows if engine_group(r["engine"]) == name])
                                           for name in ("Unity", "Unreal", "Other")}, "games": rows}


def read_snapshot(path: Path, attempts: int = 8) -> dict:
    """Copy before parsing, even on retries; never write to the source or read APK/data paths in it."""
    with tempfile.TemporaryDirectory(prefix="frameport-eval-") as tmp:
        dest = Path(tmp) / "snapshot.json"
        for attempt in range(attempts):
            shutil.copyfile(path, dest)
            try:
                return json.loads(dest.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                if attempt == attempts - 1:
                    raise
                time.sleep(0.25 * (attempt + 1))
    raise AssertionError("unreachable")


def read_dumps(path: Path) -> dict:
    records = {}
    for g in quest_dump.scan(path):
        a = analyze(g.apk, data_bytes=g.data_bytes())
        row = {"analysis": a.to_dict(), "converted": a.is_overport_output, "name": g.name}
        if a.package not in records or records[a.package]["converted"] and not row["converted"]:
            records[a.package] = row
    return records


def evaluate(snapshot: dict, entries: dict, split: str = "dev") -> dict:
    analyses = {pkg: analysis_from_dict(v["analysis"]) for pkg, v in snapshot.items() if pkg in entries}
    assignments = split_packages({pkg: a.engine for pkg, a in analyses.items()})
    records = []
    for pkg, a in sorted(analyses.items()):
        if split != "all" and assignments[pkg] != split:
            continue
        entry = entries[pkg]
        defaults = expected_defaults(a)
        records.append({"package": pkg, "title": entry.title, "engine": a.engine, "status": entry.status,
                        "converted": bool(snapshot[pkg].get("converted", a.is_overport_output)),
                        "analysis_version": snapshot[pkg].get("analysis_version"), "split": assignments[pkg],
                        "want": catalog_signature(entry, defaults, a),
                        "got": signature(engine.suggest(a, use_catalog=False), defaults)})
    return {"schema_version": 1, "split": split, "runtime_only": RUNTIME_ONLY, "snapshot_packages": len(snapshot),
            "default_policy": "per-analysis default detection, then explicit catalog removals",
            "catalog_packages": len(entries), "matched_packages": len(analyses),
            "split_assignments": assignments, **score_records(records)}


def fit_candidates(analysis, entry: catalog.CatalogEntry) -> list[dict]:
    """Use suggest's final heuristic pass, including platform filtering, non-VR and a32 normalization.

    Requiring a recommended detect() as well excludes dependencies and synthetic recipe additions. No engine
    refactor is needed: suggest() remains untouched, and catalog data is used only for the candidate exclusion and
    the hypothetical warning recipe.
    """
    suggested = engine.suggest(analysis, use_catalog=False)
    chosen = catalog_recipe(entry, expected_defaults(analysis), analysis)
    listed = set(chosen.patches) | set(entry.alt_overport)
    removed = set(entry.overport_remove) | set(entry.frame_remove) | set(entry.pcvr_remove)
    result = []
    for pid, params in sorted(suggested.patches.items()):
        p = base.get(pid)
        if (p.default_on or pid in listed or pid in removed or not base.for_game(p, analysis)
                or not p.applies(analysis)):
            continue
        s = p.detect(analysis)
        if not s or not s.recommended:
            continue
        trial = copy.deepcopy(chosen)
        trial.patches[pid] = dict(params)
        result.append({"patch": pid, "category": p.category, "experimental": p.experimental,
                       "strict": bool(getattr(p, "strict", False)), "reason": s.reason,
                       "warnings": engine.warnings(trial, analysis)})
    return result


def patch_intro_dates(cache_path: Path) -> dict:
    """One local git pickaxe per uncached patch (including missing-history results); never use the network."""
    try:
        cached = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        cached = {}
    changed = False
    for p in base.all_patches():
        if p.id in cached:
            continue
        needle = p.id.split(".", 1)[1] if p.category == "adapter" else p.id
        paths = ["src/frameport/patches/settings.py"] if p.category == "adapter" else ["src/frameport"]
        log = subprocess.run(["git", "log", f'-S"{needle}"', "--reverse", "--format=%ad", "--date=short", "--",
                              *paths], cwd=REPO, text=True, capture_output=True, check=True)
        dates = log.stdout.splitlines()
        cached[p.id] = dates[0] if dates else None
        changed = True
    if changed:
        cache_path.write_text(json.dumps(cached, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return cached


def entry_date(entry) -> str:
    return max(str(entry.verified.get("date") or ""), str(entry.updated or ""))


def fit_report(snapshot: dict, entries: dict, intro_dates: dict) -> dict:
    games, metadata = [], []
    totals = Counter()
    for pkg, entry in sorted(entries.items()):
        common = {"package": pkg, "title": entry.title, "engine": entry.engine, "xr": entry.xr,
                  "status": entry.status, "verified_date": str(entry.verified.get("date") or ""),
                  "updated": str(entry.updated or ""), "entry_date": entry_date(entry)}
        if pkg not in snapshot:
            metadata.append(common)
            continue
        candidates = fit_candidates(analysis_from_dict(snapshot[pkg]["analysis"]), entry)
        for c in candidates:
            intro = intro_dates.get(c["patch"])
            c.update(intro_date=intro,
                     newer_patch=bool(intro and common["entry_date"] and intro > common["entry_date"]))
            totals[c["patch"]] += 1
        games.append({**common, "converted": bool(snapshot[pkg].get("converted")), "candidates": candidates})
    return {"games": games, "metadata_only": metadata, "totals_by_patch": dict(sorted(totals.items())),
            "candidate_count": sum(totals.values()), "games_with_candidates": sum(bool(g["candidates"]) for g in games)}


def _md(value) -> str:
    return str(value if value is not None else "unknown").replace("|", "\\|").replace("\n", " ")


def render_fit_report(report: dict) -> str:
    lines = ["# New patch fit", "", f"{len(report['games'])} catalog games with analyses; "
             f"{report['games_with_candidates']} with candidates; {report['candidate_count']} candidate additions. "
             f"{len(report['metadata_only'])} metadata-only entries.", "",
             "Candidates are opt-in recommended patches absent from the entry, after the heuristic engine's platform "
             "and a32 rules. A listed adapter key is excluded even if its value differs. Warnings are for the full "
             "catalog recipe plus that one patch; they can include pre-existing conflicts. Unknown history/date "
             "does not imply a newer patch. Default-on patches are not candidates. All splits and statuses included.",
             "", "## Totals by patch", "", "| Patch | Games |", "| --- | ---: |"]
    lines.extend(f"| {pid} | {n} |" for pid, n in
                 sorted(report["totals_by_patch"].items(), key=lambda kv: (-kv[1], kv[0])))
    for game in report["games"]:
        lines += ["", f"## {_md(game['title'])} (`{game['package']}`)", "",
                  f"{game['engine']} / {game['xr']}; status: {game['status']}; "
                  f"entry date: {game['entry_date'] or 'unknown'}; converted: {game['converted']}.", ""]
        if not game["candidates"]:
            lines += ["No candidates."]
            continue
        lines += ["| Patch | Category | Experimental | Strict | Reason | Introduced | Newer patch | Warnings |",
                  "| --- | --- | --- | --- | --- | --- | --- | --- |"]
        for c in game["candidates"]:
            cells = [c["patch"], c["category"], c["experimental"], c["strict"], c["reason"], c["intro_date"],
                     c["newer_patch"], "; ".join(c["warnings"]) or "none"]
            lines.append("| " + " | ".join(_md(v) for v in cells) + " |")
    lines += ["", "## Catalog entries without analyses (metadata only)", "",
              "| Package | Title | Engine | XR | Status | Verified | Updated | Entry date |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for g in report["metadata_only"]:
        lines.append("| " + " | ".join(_md(g[k]) for k in
                     ("package", "title", "engine", "xr", "status", "verified_date", "updated", "entry_date")) + " |")
    return "\n".join(lines) + "\n"


def print_metrics(report: dict, verbose: bool = False) -> None:
    print(f"Split: {report['split']}; catalog/analysis intersection: {report['matched_packages']}; "
          f"converted: {report['converted_games']}; unsupported excluded: {report['excluded_unsupported']}")
    print(f"{'Engine':10} {'Games':>5} {'Exact':>9} {'Fields':>11} {'Prec':>7} {'Recall':>7} {'F1':>7} {'Runtime':>9}")
    for name, metrics in [("All", report), *report["by_engine"].items()]:
        h, r = metrics["headline"], metrics["runtime"]
        fields = h["fields"].values()
        f = f"{sum(x['matched'] for x in fields)}/{sum(x['total'] for x in h['fields'].values())}"
        p = h["patches"]
        print(f"{name:10} {h['games']:5} {h['exact']:4}/{h['games']:<4} {f:>11} "
              f"{p['precision']:7.1%} {p['recall']:7.1%} {p['f1']:7.1%} {r['exact']:4}/{r['games']:<4}")
    for name, metrics in [("All", report), *report["by_engine"].items()]:
        print(f"\n{name}: field matches (headline / runtime)")
        for scope in ("headline", "runtime"):
            matches = metrics[scope]["fields"].items()
            print(scope + ": " + ", ".join(f"{k}={v['matched']}/{v['total']}" for k, v in matches))
        print(f"{'Scope / patch':55} {'TP':>5} {'FP':>5} {'FN':>5} {'Prec':>7} {'Recall':>7} {'F1':>7}")
        for scope in ("headline", "runtime"):
            for pid, p in metrics[scope]["per_patch"].items():
                print(f"{scope + ' / ' + pid:55} {p['tp']:5} {p['fp']:5} {p['fn']:5} "
                      f"{p['precision']:7.1%} {p['recall']:7.1%} {p['f1']:7.1%}")
    u = report["unsupported"]["headline"]
    print(f"\nUnsupported (excluded): {u['exact']}/{u['games']} exact; patch F1={u['patches']['f1']:.1%}")
    for game in report["games"]:
        s = game["score"]
        if verbose or not s["headline"]["exact"] or not s["runtime"]["exact"] or game["status"] == "unsupported":
            print(f"\n{game['package']} [{game['engine']}, {game['status']}, {game['split']}"
                  + (", converted" if game["converted"] else "") + "]")
            for scope in ("headline", "runtime"):
                for diff in ("missing", "extra"):
                    if s[scope][diff]:
                        print(f"  {scope} {diff}: {json.dumps(s[scope][diff], sort_keys=True)}")
                if not s[scope]["fields"].get("status_unsupported", True):
                    print(f"  {scope} status_unsupported: catalog={game['want']['status_unsupported']} "
                          f"heuristics={game['got']['status_unsupported']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", type=Path, nargs="?")
    ap.add_argument("--snapshot", type=Path)
    ap.add_argument("--split", choices=("dev", "holdout", "all"), default="dev")
    ap.add_argument("--json", type=Path, dest="json_out")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--fit-report", action="store_true")
    ap.add_argument("--out", type=Path, help="Markdown destination for --fit-report (otherwise stdout)")
    args = ap.parse_args()
    if not args.snapshot and not args.dumps:
        ap.error("provide a dumps folder or --snapshot")
    if args.out and not args.fit_report:
        ap.error("--out requires --fit-report")
    snapshot = read_snapshot(args.snapshot) if args.snapshot else read_dumps(args.dumps)
    # Explicitly bundled entries: remote caches and personal recipes must not affect this benchmark.
    entries = catalog._load_dir(REPO / "catalog/games", "bundled")
    if args.fit_report:
        cache = (args.snapshot.parent if args.snapshot else args.dumps) / "patch-intro-dates.json"
        report = fit_report(snapshot, entries, patch_intro_dates(cache))
        markdown = render_fit_report(report)
        if args.out:
            args.out.write_text(markdown, encoding="utf-8")
            print(f"{report['candidate_count']} candidates in {report['games_with_candidates']} games -> {args.out}")
        else:
            print(markdown, end="")
    else:
        report = evaluate(snapshot, entries, args.split)
        print_metrics(report, args.verbose)
    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")
    # Retain the legacy evaluator's nonzero exit for recipe differences; JSON is still written.
    return 0 if args.fit_report or report["headline"]["games"] == report["headline"]["exact"] else 1


if __name__ == "__main__":
    sys.exit(main())
