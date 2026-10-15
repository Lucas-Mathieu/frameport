"""Checks against real game dumps (opt-in): FRAMEPORT_GAMES=<folder with dumps> pytest -m games"""
import pytest
from conftest import games_dir

from frameport.analysis.detect import analyze
from frameport.recommend import catalog
from frameport.sources import quest_dump

pytestmark = pytest.mark.games

# 2026-10-15 snapshot baseline: 3/65 supported games exact, patch F1=240/495; rounded down to two decimals.
# See scripts/eval_heuristics.py; runtime-only choices and unsupported catalog entries are excluded.
MIN_HEURISTICS_EXACT_RATIO = 0.07
MIN_HEURISTICS_PATCH_F1 = 0.65


@pytest.fixture(scope="module")
def dumps():
    d = games_dir()
    if not d:
        pytest.skip("set FRAMEPORT_GAMES")
    return quest_dump.scan(d)


def test_every_catalog_game_found_and_detected(dumps):
    by_pkg = {}
    for g in dumps:
        a = analyze(g.apk, deep=False)
        by_pkg[a.package] = a
    matched = [pkg for pkg in catalog.load() if pkg in by_pkg]
    assert len(matched) >= min(len(by_pkg), 1), "no catalog game found in the dumps"
    for pkg, entry in catalog.load().items():
        if pkg not in by_pkg:
            continue
        a = by_pkg[pkg]
        assert a.engine == entry.engine, pkg
        assert a.xr == entry.xr, pkg
        if entry.status == "unsupported" and "32-bit" in entry.notes:
            assert a.only_32bit, pkg
    print(f"checked {len(matched)} catalog games")


def test_heuristics_reproduce_catalog(dumps):
    """Ratchet catalog-only static accuracy without requiring runtime findings to be detectable from APKs."""
    import importlib.util
    from pathlib import Path

    script = Path(__file__).resolve().parents[1] / "scripts/eval_heuristics.py"
    spec = importlib.util.spec_from_file_location("ev", script)
    ev = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ev)
    snapshot = {}
    for g in dumps:
        a = analyze(g.apk, data_bytes=g.data_bytes())
        row = {"analysis": a.to_dict(), "converted": a.is_overport_output}
        if a.package not in snapshot or snapshot[a.package]["converted"] and not row["converted"]:
            snapshot[a.package] = row
    entries = catalog._load_dir(script.parents[1] / "catalog/games", "bundled")
    result = ev.evaluate(snapshot, entries, split="all")["headline"]
    assert result["games"], "no supported catalog games found in the dumps"
    assert result["exact_ratio"] >= MIN_HEURISTICS_EXACT_RATIO, (
        f"only {result['exact']}/{result['games']} static catalog recipes reproduced exactly")
    assert result["patches"]["f1"] >= MIN_HEURISTICS_PATCH_F1, result["patches"]
