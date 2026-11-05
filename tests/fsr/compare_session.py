"""Summarize a live A/B FSR session, excluding transition/menu settling time.

python tests/fsr/compare_session.py capture.csv --settle 3 --trim-end 1
"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict

from compare_metrics import percentile


def summarize(path, settle, trim_end):
    groups = defaultdict(list)
    with open(path, encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            if row.get("layers") != "1":
                continue
            if row.get("target") == "applet":
                continue
            if "target" not in row and row.get("filter") not in ("5", "6"):
                continue
            try:
                key = tuple(row.get(k, "unknown") for k in ("backend", "segment", "implementation", "filter",
                    "input_width", "input_height", "width", "height", "selected_filter",
                    "sharpness_stored", "cas_strength", "viewport_width", "viewport_height", "downscale_filter", "scale_mode"))
                float(row["elapsed_s"])
                float(row["gpu_ms"])
                float(row["frame_interval_ms"])
            except (KeyError, ValueError, TypeError):
                continue  # A live reader may encounter an unfinished final row.
            groups[key].append(row)
    report = []
    for key, rows in groups.items():
        rows.sort(key=lambda r: int(r["frame"]))
        start = float(rows[0]["elapsed_s"]) + settle
        end = float(rows[-1]["elapsed_s"]) - trim_end
        selected = [r for r in rows if start <= float(r["elapsed_s"]) <= end]
        if len(selected) < 30:
            continue
        entry = dict(zip(("backend", "segment", "implementation", "filter", "input_width",
                          "input_height", "output_width", "output_height", "selected_filter",
                          "sharpness_stored", "cas_strength", "viewport_width", "viewport_height", "downscale_filter", "scale_mode"), key))
        labels = {"0": "Nearest", "1": "Bilinear", "2": "Bicubic", "3": "Gaussian",
                  "4": "ScaleForce", "5": "FSR", "6": "FSR EASU", "7": "Catmull-Rom",
                  "8": "Catmull-Rom fast", "9": "ScaleForce fast", "16": "Nearest + CAS", "17": "Bilinear + CAS",
                  "18": "Catmull-Rom + CAS", "19": "Catmull-Rom fast + CAS", "20": "Area",
                  "10": "Bilinear + CAS", "11": "Catmull-Rom + CAS"}
        entry["filter_name"] = labels.get(entry["filter"], entry["filter"])
        if entry["filter"] == "9" and entry["selected_filter"] == "0":
            entry["filter_name"] = "Nearest + CAS"  # Old diagnostic-only ID, never persisted.
        entry.update(samples=len(selected), duration_s=end-start)
        for metric in ("cpu_ms", "gpu_ms", "frame_interval_ms"):
            values = [float(r[metric]) for r in selected if math.isfinite(float(r[metric]))
                      and float(r[metric]) > 0]
            entry[metric] = ({"samples": len(values), "mean": statistics.mean(values),
                              "median": statistics.median(values), "p95": percentile(values, .95),
                              "p99": percentile(values, .99)} if values else None)
        cadence = entry["frame_interval_ms"]
        entry["submission_fps"] = 1000/cadence["mean"] if cadence else None
        entry["offscreen_passes"] = sorted({int(r["offscreen_passes"]) for r in selected})
        report.append(entry)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture")
    parser.add_argument("--settle", type=float, default=3)
    parser.add_argument("--trim-end", type=float, default=1)
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.settle < 0 or args.trim_end < 0:
        parser.error("settle and trim-end must be nonnegative")
    data = json.dumps(summarize(args.capture, args.settle, args.trim_end), indent=2, allow_nan=False)
    print(data)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as target:
            target.write(data+"\n")
