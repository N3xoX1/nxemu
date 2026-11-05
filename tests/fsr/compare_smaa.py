#!/usr/bin/env python3
"""Compare benchmark RGB8 captures. Differences are not a quality score.

Requires NumPy. A sparse edge error can be hidden by a tiny whole-image MAE;
always inspect maximum differences and the actual contour crops as well.
"""
import argparse
import csv
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('reference', type=Path)
parser.add_argument('candidate', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--max-error', type=int)
args = parser.parse_args()
rows = []
for reference in sorted(args.reference.glob('*-SMAA.ppm')):
    candidate = args.candidate / reference.name
    def read(path):
        magic, dimensions, maximum, data = path.read_bytes().split(b'\n', 3)
        if magic != b'P6' or maximum != b'255':
            raise ValueError(f'Unsupported capture: {path}')
        width, height = map(int, dimensions.split())
        pixels = np.frombuffer(data, dtype=np.uint8)
        if pixels.size != width * height * 3:
            raise ValueError(f'Unexpected pixel count: {path}')
        return (width, height), pixels.astype(np.int16)
    dimensions, a = read(reference)
    other_dimensions, b = read(candidate)
    if dimensions != other_dimensions:
        raise ValueError(f'Mismatched dimensions: {reference.name}')
    difference = np.abs(a - b)
    changed = int(np.count_nonzero(difference))
    maximum = int(difference.max())
    rows.append([reference.stem, changed, changed / difference.size * 100,
                 maximum, float(difference.mean())])
    if args.max_error is not None and maximum > args.max_error:
        raise ValueError(f'{reference.name}: maximum RGB8 difference {maximum} exceeds {args.max_error}')
if not rows:
    raise SystemExit('No SMAA captures found')
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('w', newline='', encoding='utf-8') as file:
    writer = csv.writer(file)
    writer.writerow(['case', 'changed_rgb_channels', 'changed_percent', 'max_error_255', 'mae_255'])
    writer.writerows(rows)
print(f'Compared {len(rows)} captures: {args.output}')
