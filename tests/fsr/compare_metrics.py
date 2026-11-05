"""Compare opt-in presentation CSVs. Python standard library only.

Usage: python tests/fsr/compare_metrics.py before.csv after.csv --warmup 120
"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict


def percentile(values, q):
    ordered = sorted(values)
    index = (len(ordered) - 1) * q
    lo = int(index)
    return ordered[lo] + (ordered[math.ceil(index)] - ordered[lo]) * (index - lo)


def read(path, warmup, selected_filter):
    groups = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            if int(row["filter"]) != selected_filter:
                continue  # Excludes bilinear applet captures.
            key = (row["backend"], int(row["width"]), int(row["height"]), int(row["layers"]))
            groups[key].append(row)
    result = {}
    for key, rows in groups.items():
        rows.sort(key=lambda r: int(r["frame"]))  # Queries can complete out of order.
        rows = rows[warmup:]
        if len(rows) < 30:
            raise ValueError(f"{path}: {key}: only {len(rows)} samples after warmup")
        data = {"frames": len(rows), "implementation": sorted({r["implementation"] for r in rows})}
        for metric in ("cpu_ms", "gpu_ms"):
            values = [float(row[metric]) for row in rows if math.isfinite(float(row[metric]))]
            data[metric] = ({"samples": len(values), "mean": statistics.mean(values),
                             "median": statistics.median(values), "p95": percentile(values, .95),
                             "p99": percentile(values, .99)} if values else None)
        data["gpu_missing"] = len(rows) - (data["gpu_ms"]["samples"] if data["gpu_ms"] else 0)
        result[key] = data
    if not result:
        raise ValueError(f"{path}: no samples for filter {selected_filter}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--filter", type=int, default=5)
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("warmup must be nonnegative")
    before = read(args.before, args.warmup, args.filter)
    after = read(args.after, args.warmup, args.filter)
    if set(before) != set(after):
        raise ValueError("Backend, resolution or layer counts differ; record matching runs")
    report = []
    for key in before:
        row = {"configuration": key, "before": before[key], "after": after[key]}
        for metric in ("cpu_ms", "gpu_ms"):
            b, a = before[key][metric], after[key][metric]
            if b and a and b["median"] > 0:
                row[metric + "_median_reduction_percent"] = 100 * (1 - a["median"] / b["median"])
        report.append(row)
    output = json.dumps(report, indent=2, allow_nan=False)
    print(output)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as target:
            target.write(output + "\n")


if __name__ == "__main__":
    main()
