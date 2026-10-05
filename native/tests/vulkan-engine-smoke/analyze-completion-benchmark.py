"""Describe serial encoder A/B samples; never grants a performance Gate.

Run after verify-completion-benchmark.ps1 -EncoderMatrix succeeds. No samples
are dropped. Paired comparisons hold completion backend and repeat fixed.
"""
import argparse
import csv
import json
import math
import re
import statistics
from pathlib import Path


def stats(values):
    ordered = sorted(values)
    median = statistics.median(ordered)
    return {
        "count": len(values), "mean": statistics.fmean(values), "median": median,
        "p95": ordered[math.ceil(.95 * len(ordered)) - 1],
        "p99": ordered[math.ceil(.99 * len(ordered)) - 1],
        "stddev_population": statistics.pstdev(values),
        "mad": statistics.median(abs(x - median) for x in values),
    }


def read_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"Missing samples: {path}")
    columns = [key for key in rows[0] if key != "frame"]
    result = {}
    for key in columns:
        values = [float(row[key]) for row in rows]
        if any(not math.isfinite(x) or x < 0 for x in values):
            raise ValueError(f"Invalid numeric samples: {path} {key}")
        result[key] = stats(values)
    return result


def analyze(root):
    with (root / "summary.csv").open(newline="", encoding="utf-8-sig") as stream:
        summary = list(csv.DictReader(stream))
    modes = {row["Validation"] for row in summary}
    if len(modes) != 1:
        raise ValueError("Expected one validation policy in the verified summary")
    runs = []
    by_key = {}
    pattern = re.compile(r"([0-9]+)-(fence|timeline)-(legacy|submit2)-(legacy|sync2)\.csv")
    for path in sorted(root.glob("*.csv")):
        match = pattern.fullmatch(path.name)
        if not match:
            continue
        repeat, backend, submit, barrier = match.groups()
        key = (int(repeat), backend, submit, barrier)
        metrics = read_rows(path)
        phases = read_rows(Path(str(path) + ".phases.csv"))
        if any(x["count"] != metrics["wall_ms"]["count"] for x in phases.values()):
            raise ValueError(f"Phase count mismatch: {path}")
        metrics.update(phases)
        run = {"repeat": key[0], "backend": backend, "submit": submit,
               "barrier": barrier, "file": path.name, "metrics": metrics}
        runs.append(run)
        by_key[key] = run
    if not runs:
        raise ValueError("No encoder-labelled sample files")
    # Only analyze cases actually accepted by the shared PowerShell judge.
    verified = {(int(row["Repeat"]), row["Backend"], row["Submit"], row["Barrier"])
                for row in summary}
    if set(by_key) != verified:
        raise ValueError("Raw runs and verified summary disagree")
    repeats = sorted({key[0] for key in by_key})
    expected = {(r, b, s, a) for r in repeats for b in ("fence", "timeline")
                for s in ("legacy", "submit2") for a in ("legacy", "sync2")}
    if set(by_key) != expected:
        raise ValueError("Analysis requires a complete EncoderMatrix")
    comparisons = []
    for backend in ("fence", "timeline"):
        for submit, barrier in (("submit2", "legacy"), ("legacy", "sync2"), ("submit2", "sync2")):
            changes = []
            for repeat in repeats:
                baseline = by_key[(repeat, backend, "legacy", "legacy")]["metrics"]
                current = by_key[(repeat, backend, submit, barrier)]["metrics"]
                change = {metric: {
                    stat: (current[metric][stat] / baseline[metric][stat] - 1) * 100
                    if baseline[metric][stat] > 0 else None
                    for stat in ("median", "p95", "p99")}
                    for metric in ("wall_ms", "record_ms", "submit_ms", "present_ms", "gpu_clear_ms")}
                changes.append({"repeat": repeat, "percent_change": change})
            wall = {stat: stats([c["percent_change"]["wall_ms"][stat] for c in changes])
                    for stat in ("median", "p95", "p99")}
            flags = [c["repeat"] for c in changes if any(
                c["percent_change"]["wall_ms"][stat] > 3 for stat in ("median", "p95"))]
            comparisons.append({"backend": backend, "submit": submit, "barrier": barrier,
                                "baseline": "legacy/legacy with same backend and repeat",
                                "paired_changes": changes, "wall_change_dispersion": wall,
                                "rounds_above_3pct_investigation_threshold": flags})
    return {
        "scope": "Finite instrumented Win32 clear; serial paired comparisons, no sample rejection",
        "performance_gate": "NOT ACCEPTED: analysis is not game/visual/platform evidence",
        "validation": next(iter(modes)), "repeats": len(repeats), "runs": runs,
        "desktop_sampling_minimum_met": len(repeats) >= 5 and all(
            r["metrics"]["wall_ms"]["count"] >= 1000 for r in runs),
        "limitations": ["3% flags require investigation, not automatic causal attribution",
                        "WSI pacing, engine cleanup and observer/hook overhead remain included",
                        "Validation OFF retains CC_DEBUG_FORCE and gfx debug validation",
                        "Thread CPU timing quantization limits per-frame conclusions"],
        "comparisons": comparisons,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    result = analyze(args.root)
    output = args.root / "encoder-comparison.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Analysis: {output}; performance Gate remains unaccepted")
    for comparison in result["comparisons"]:
        median = comparison["wall_change_dispersion"]["median"]["median"]
        p95 = comparison["wall_change_dispersion"]["p95"]["median"]
        flags = comparison["rounds_above_3pct_investigation_threshold"]
        print(f"{comparison['backend']} {comparison['submit']}/{comparison['barrier']}: "
              f"paired wall median change median={median:+.2f}%, "
              f"P95 change median={p95:+.2f}%, investigate rounds={flags}")
