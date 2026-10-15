"""Synthetic evaluator tests: no APKs, snapshots, git history, or network needed."""
import importlib.util
import json
from pathlib import Path

import pytest

from frameport.core.library import analysis_from_dict
from frameport.patches import base
from frameport.recommend import catalog, engine

script = Path(__file__).parents[1] / "scripts/eval_heuristics.py"
spec = importlib.util.spec_from_file_location("eval_heuristics", script)
ev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ev)


def _analysis(**kwargs):
    fields = dict(package="com.example.game", engine="Unity", xr="OpenXR", abis=["arm64-v8a"],
                  libs=[], extra={}, graphics="Vulkan")
    fields.update(kwargs)
    return analysis_from_dict(fields)


def _entry(**kwargs):
    return catalog.CatalogEntry(**(dict(package="com.example.game", title="Example", status="works") | kwargs))


def _patch(monkeypatch, pid, *, category="frame", default=False, applies=True, recommended=True,
           a32=None, requires=(), conflicts=()):
    base.load_all()
    p = base.Patch()
    p.id, p.title, p.category, p.default_on = pid, pid, category, default
    p.a32, p.requires, p.conflicts = a32, requires, conflicts
    p.experimental, p.strict = True, True
    p.applies = lambda _: applies
    params = {"value": 3} if category == "adapter" else {}
    p.detect = lambda _: base.Suggestion(recommended, f"reason for {pid}", params)
    monkeypatch.setitem(base.REGISTRY, pid, p)
    return p


def test_catalog_signature_is_catalog_only(monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("catalog signature must not call suggest")

    monkeypatch.setattr(engine, "suggest", forbidden)
    a = _analysis()
    defaults = ev.expected_defaults(a)
    entry = _entry(overport_extra=["patch_remove_unreal_force_quit"], overport_remove=["patch_copy_libraries"],
                   frame=["frame.unity_text_input"], frame_remove=["frame.adapter"],
                   adapter={"scene_emul": 0, "scale": 1.5}, device=["device.text_input_window"],
                   device_files={"user.cfg": "r_vsync=0"}, alt_overport=["patch_fix_ue4_crash"], use_alt=True,
                   foveation="fixed", foveation_radius=0.5, foveation_test=True,
                   lepton_env={"X": "1"}, config_sync={"user.cfg": {"width": "w"}})
    want = ev.catalog_signature(entry, defaults)
    assert want["overport"] == ["patch_remove_unreal_force_quit"]
    assert want["overport_removed"] == ["patch_copy_libraries"]
    assert want["frame"] == ["frame.unity_text_input"]
    assert want["frame_removed"] == ["frame.adapter"]
    assert want["adapter"] == {"scene_emul": 0, "scale": 1.5}
    assert want["device"] == ["device.text_input_window"]
    assert want["device_files"] == ["user.cfg"]
    assert want["alt"] == ["patch_fix_ue4_crash"]
    assert want["use_alt"] is True
    assert want["lepton_env"] == {"X": "1"}
    assert want["config_sync"] == {"user.cfg": {"width": "w"}}
    assert want["foveation"] == {"mode": "fixed", "radius": 0.5, "test": True}
    assert not want["status_unsupported"]


def test_mask_conditional_defaults_and_score_explicit_removals():
    a = _analysis()
    defaults = ev.expected_defaults(a)
    assert "frame.ltw_depth" not in defaults
    assert "frame.vk_sanitize" not in defaults
    assert "frame.adapter" in defaults
    r = ev.catalog_recipe(_entry(), defaults)
    assert ev.signature(r, defaults)["frame_removed"] == []
    removed = ev.catalog_signature(_entry(frame_remove=["frame.adapter", "frame.ltw_depth"]), defaults)
    assert removed["frame_removed"] == ["frame.adapter"]
    scored = ev.score_signatures(removed, ev.signature(r, defaults))["headline"]
    assert scored["missing"] == {"remove:frame.adapter": True}
    assert not scored["exact"]
    r.patches.pop("frame.adapter")
    dropped = ev.score_signatures(ev.catalog_signature(_entry(), defaults), ev.signature(r, defaults))["headline"]
    assert dropped["extra"] == {"remove:frame.adapter": True}


def test_catalog_adapter_does_not_inherit_heuristics():
    a = _analysis(libs=["libOVRPlugin.so"])
    got = engine.suggest(a, use_catalog=False)
    assert "adapter.haptic_fix" in got.patches
    assert ev.catalog_signature(_entry(), ev.expected_defaults(a))["adapter"] == {}


def test_scorer_values_counts_and_exact():
    want = ev.catalog_signature(_entry(frame=["frame.unity_text_input"], adapter={"scene_emul": 1}))
    got = ev.catalog_signature(_entry(frame=["frame.unity_text_input", "frame.hw_video_decode"],
                                      adapter={"scene_emul": 2}))
    score = ev.score_signatures(want, got)["headline"]
    assert not score["exact"]
    assert score["per_patch"]["frame.unity_text_input"] == ev._rates(1, 0, 0)
    assert score["per_patch"]["frame.hw_video_decode"] == ev._rates(0, 1, 0)
    assert score["per_patch"]["adapter.scene_emul"] == ev._rates(0, 1, 1)
    assert score["missing"] == {"adapter.scene_emul": 1}
    assert score["extra"] == {"adapter.scene_emul": 2, "frame.hw_video_decode": True}
    same = ev.score_signatures(want, want)
    assert same["headline"]["exact"] and same["runtime"]["exact"]
    assert same["headline"]["per_patch"]["adapter.scene_emul"] == ev._rates(1, 0, 0)


def test_runtime_separate_from_headline():
    want = ev.catalog_signature(_entry(adapter={"scale": 1.5, "sync_guard": 1}, use_alt=True,
                                       lepton_env={"X": "Y"}, config_sync={"foo": {"w": "w"}}))
    got = ev.catalog_signature(_entry())
    score = ev.score_signatures(want, got)
    assert score["headline"]["exact"]
    assert not score["headline"]["missing"]
    assert not score["runtime"]["exact"]
    assert score["runtime"]["per_patch"]["adapter.scale"] == ev._rates(0, 0, 1)
    assert score["runtime"]["missing"]["use_alt"]


def test_aggregate_excludes_unsupported_and_breaks_down_engines():
    want = ev.catalog_signature(_entry(frame=["frame.unity_text_input"]))
    empty = ev.catalog_signature(_entry())
    rows = [dict(package="com.a", status="works", engine="Unity", converted=True, want=want, got=want),
            dict(package="com.b", status="issues", engine="Unreal", want=want, got=empty),
            dict(package="com.c", status="unsupported", engine="Other", want=want, got=empty)]
    result = ev.score_records(rows)
    assert result["headline"]["games"] == 2
    assert result["headline"]["exact"] == 1
    assert result["headline"]["exact_ratio"] == 0.5
    assert result["headline"]["patches"] == ev._rates(1, 0, 1)
    assert result["headline"]["fields"]["frame"] == {"matched": 1, "total": 2}
    assert result["runtime"]["exact"] == 2
    assert result["excluded_unsupported"] == 1
    assert result["unsupported"]["headline"]["patches"] == ev._rates(0, 0, 1)
    assert result["by_engine"]["Unreal"]["headline"]["patches"] == ev._rates(0, 0, 1)
    assert result["converted_games"] == 1
    assert len(result["games"]) == 3


def test_split_deterministic_stratified_and_disjoint():
    engines = {f"com.example.{name}{i}": name for name in ("Unity", "Unreal", "Other") for i in range(10)}
    assignments = ev.split_packages(engines)
    assert assignments == ev.split_packages(dict(reversed(list(engines.items()))))
    for name in ("Unity", "Unreal", "Other"):
        mine = [pkg for pkg in engines if engines[pkg] == name]
        ranked = sorted(mine, key=lambda p: ev.hashlib.sha256(p.encode()).digest())
        assert {p for p in mine if assignments[p] == "holdout"} == set(ranked[:2])
    dev = {p for p, s in assignments.items() if s == "dev"}
    holdout = {p for p, s in assignments.items() if s == "holdout"}
    assert not dev & holdout
    assert dev | holdout == engines.keys()
    assert ev.split_packages({}) == {}
    assert ev.split_packages({"com.one": "CryEngine"}) == {"com.one": "dev"}


def test_fit_selects_only_recommended_opt_ins_not_listed_or_removed(monkeypatch):
    _patch(monkeypatch, "frame.eval_candidate", conflicts=("frame.adapter",), requires=("frame.eval_unrecommended",))
    _patch(monkeypatch, "frame.eval_default", default=True)
    _patch(monkeypatch, "frame.eval_unrecommended", recommended=False)
    _patch(monkeypatch, "frame.eval_inapplicable", applies=False)
    _patch(monkeypatch, "frame.eval_listed")
    _patch(monkeypatch, "frame.eval_removed")
    _patch(monkeypatch, "frame.eval_alternate")
    _patch(monkeypatch, "adapter.eval_setting", category="adapter")
    _patch(monkeypatch, "pcvr.eval_wrong_platform", category="pcvr")
    entry = _entry(frame=["frame.eval_listed"], frame_remove=["frame.eval_removed"],
                   alt_overport=["frame.eval_alternate"], adapter={"eval_setting": 9})
    candidates = {r["patch"]: r for r in ev.fit_candidates(_analysis(), entry)}
    assert "frame.eval_candidate" in candidates
    assert not set(candidates) & {"frame.eval_default", "frame.eval_unrecommended", "frame.eval_inapplicable",
                                  "frame.eval_listed", "frame.eval_removed", "frame.eval_alternate",
                                  "adapter.eval_setting", "pcvr.eval_wrong_platform"}
    c = candidates["frame.eval_candidate"]
    assert c["reason"] == "reason for frame.eval_candidate"
    assert c["category"] == "frame" and c["experimental"] and c["strict"]
    assert any("conflicts" in w for w in c["warnings"])


def test_fit_uses_a32_and_flat_rules(monkeypatch):
    _patch(monkeypatch, "frame.eval_64_only", a32=False)
    _patch(monkeypatch, "frame.eval_32_ok", a32=True)
    candidates = {r["patch"] for r in ev.fit_candidates(_analysis(abis=["armeabi-v7a"]), _entry())}
    assert "frame.eval_32_ok" in candidates
    assert "frame.eval_64_only" not in candidates
    assert "frame.a32_bridge" not in candidates  # implied for 32-bit entries, as engine._a32_predates_bridge does
    flat = {r["patch"] for r in ev.fit_candidates(_analysis(extra={"vr_kind": "none"}), _entry())}
    assert "frame.eval_32_ok" not in flat  # needs_vr defaults True, removed by engine._non_quest


def test_fit_metadata_dates_and_converted(monkeypatch):
    monkeypatch.setattr(ev, "fit_candidates", lambda a, e: [dict(patch="frame.new", category="frame",
                                                               reason="x", experimental=False, strict=False,
                                                               warnings=[])])
    entries = {"com.a": _entry(package="com.a", verified={"date": "2026-01-01"}, updated="2026-02-01"),
               "com.b": _entry(package="com.b", status="unsupported", engine="Other", xr="VrApi")}
    report = ev.fit_report({"com.a": {"analysis": _analysis(package="com.a").to_dict(), "converted": True}}, entries,
                           {"frame.new": "2026-03-01"})
    assert report["games"][0]["candidates"][0]["newer_patch"] is True
    assert report["games"][0]["entry_date"] == "2026-01-01"  # verified, not updated
    assert report["games"][0]["converted"]
    assert report["metadata_only"][0]["package"] == "com.b"
    assert report["metadata_only"][0]["xr"] == "VrApi"
    assert report["totals_by_patch"] == {"frame.new": 1}
    md = ev.render_fit_report(report)
    assert "metadata only" in md and "com.b" in md and "frame.new" in md


def test_snapshot_copied_before_read_and_retried(tmp_path, monkeypatch):
    source = tmp_path / "snapshot.json"
    source.write_text('{"complete": true}')
    calls = []
    real_copy = ev.shutil.copyfile

    def copying(src, dest):
        calls.append((src, dest))
        real_copy(src, dest)
        if len(calls) == 1:
            dest.write_text('{"partial":')

    monkeypatch.setattr(ev.shutil, "copyfile", copying)
    monkeypatch.setattr(ev.time, "sleep", lambda _: None)
    assert ev.read_snapshot(source) == {"complete": True}
    assert len(calls) == 2
    assert all(src == source and dest != source for src, dest in calls)
    assert json.loads(source.read_text()) == {"complete": True}


def test_intro_dates_cached_and_adapter_searches_bare_key(tmp_path, monkeypatch):
    calls = []
    patch = base.get("adapter.scene_emul")
    monkeypatch.setattr(base, "all_patches", lambda: [patch])

    def log(args, **kwargs):
        calls.append(args)
        return type("Result", (), {"stdout": "2026-01-02\n2026-02-03\n"})()

    monkeypatch.setattr(ev.subprocess, "run", log)
    cache = tmp_path / "patch-intro-dates.json"
    assert ev.patch_intro_dates(cache) == {"adapter.scene_emul": "2026-01-02"}
    assert '-S"scene_emul"' in calls[0]
    assert calls[0][-1] == "src/frameport/patches/settings.py"
    assert ev.patch_intro_dates(cache) == {"adapter.scene_emul": "2026-01-02"}
    assert len(calls) == 1


def test_legacy_dumps_cli_remains_available(tmp_path, monkeypatch, capsys):
    a = _analysis()
    snapshot = {a.package: {"analysis": a.to_dict()}}
    entry = _entry()
    calls = []
    monkeypatch.setattr(ev, "read_dumps", lambda p: calls.append(p) or snapshot)
    monkeypatch.setattr(catalog, "_load_dir", lambda *args: {a.package: entry})
    monkeypatch.setattr(ev.sys, "argv", ["eval_heuristics.py", str(tmp_path), "--verbose"])
    assert ev.main() in (0, 1)  # a difference exit is compatible with the original CLI
    assert calls == [tmp_path]
    output = capsys.readouterr().out
    assert "Split: dev" in output and a.package in output


def test_snapshot_fit_cli_writes_markdown_and_json(tmp_path, monkeypatch):
    a = _analysis()
    snapshot = tmp_path / "synthetic.json"
    snapshot.write_text(json.dumps({a.package: {"analysis": a.to_dict(), "converted": True}}))
    md, js = tmp_path / "fit.md", tmp_path / "fit.json"
    monkeypatch.setattr(catalog, "_load_dir", lambda *args: {a.package: _entry()})
    monkeypatch.setattr(ev, "patch_intro_dates", lambda p: {})
    monkeypatch.setattr(ev.quest_dump, "scan", lambda *a: pytest.fail("must not scan with --snapshot"))
    monkeypatch.setattr(ev.sys, "argv", ["eval_heuristics.py", "--snapshot", str(snapshot), "--fit-report",
                                        "--out", str(md), "--json", str(js)])
    assert ev.main() == 0
    assert "# New patch fit" in md.read_text()
    assert json.loads(js.read_text())["games"][0]["converted"]


def test_evaluate_split_coverage_and_no_snapshot_analysis_mutation():
    snapshot = {f"com.game.n{i}": {"analysis": _analysis(package=f"com.game.n{i}").to_dict(), "converted": i == 0}
                for i in range(10)}
    entries = {p: _entry(package=p) for p in snapshot}
    before = json.dumps(snapshot, sort_keys=True)
    reports = {s: ev.evaluate(snapshot, entries, s) for s in ("all", "dev", "holdout")}
    assert reports["dev"]["total_games"] == 8
    assert reports["holdout"]["total_games"] == 2
    assert reports["all"]["total_games"] == 10
    assert json.dumps(snapshot, sort_keys=True) == before
    assert reports["all"]["converted_games"] == 1


def test_catalog_truth_applies_the_a32_bridge_transform():
    """A 32-bit game's entry relies on the bridge switching OVRPort and the 64-bit fixes off; the heuristics do the
    same, so neither side may count those as removals."""
    a = _analysis(abis=["armeabi-v7a"], engine="Unreal", graphics="GLES")
    defaults = ev.expected_defaults(a)
    want = ev.catalog_signature(_entry(), defaults, a)
    got = ev.signature(engine.suggest(a, use_catalog=False), defaults)
    assert want["overport_removed"] == got["overport_removed"]
    assert want["frame_removed"] == got["frame_removed"]


def test_dated_score_sets_aside_patches_newer_than_the_entry():
    want = ev.catalog_signature(_entry(frame=["frame.unity_text_input"]))
    got = ev.catalog_signature(_entry(frame=["frame.unity_text_input", "frame.hw_video_decode"]))
    strict = ev.score_signatures(want, got)["headline"]
    dated = ev.score_signatures(want, got, {"frame.hw_video_decode"})["headline"]
    assert strict["extra"] and not strict["exact"]
    assert dated["exact"] and dated["gap"] == {"frame.hw_video_decode": True}
    assert "frame.hw_video_decode" not in dated["per_patch"]


def test_inherited_picks_are_adapter_and_untoggled_device_patches():
    inherited = ev.inherited_decisions()
    assert "adapter.haptic_fix" in inherited
    assert "device.text_input_window" not in inherited  # catalog.TOGGLED_DEVICE: dropped unless the entry names it
    assert not any(pid.startswith("frame.") for pid in inherited)


def test_gap_decisions_use_the_verified_date_not_updated():
    e = _entry(verified={"date": "2026-09-28"}, updated="2026-10-14")
    gaps = ev.gap_decisions(e, {"frame.unity_text_input": "2026-10-03", "frame.adapter": "2026-09-28",
                                "patch_remove_unreal_force_quit": "2026-10-01"})
    assert gaps == {"frame.unity_text_input", "alt:patch_remove_unreal_force_quit"}
    assert ev.entry_date(e) == "2026-09-28"
