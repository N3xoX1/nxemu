"""Independent reference for area resampling with arbitrary subpixel phase.
Tests the exact 2x2-bilinear factoring and the sampler-edge regression.
Run: python test_area_unaligned.py
"""
import math
import random
from collections import defaultdict

import numpy as np

R = random.Random(0xA2EA)
SRC = np.array([[R.random() for _ in range(31)] for _ in range(29)], dtype=np.float64)
H, W = SRC.shape


def area(a, b, lo, hi):
    return max(0.0, min(b, hi) - max(a, lo))


def sample(ix, iy):
    return SRC[min(max(iy, 0), H - 1), min(max(ix, 0), W - 1)]


def reference(ax, ay, wx, wy):
    val = 0.0
    for iy in range(math.floor(ay), math.ceil(ay + wy)):
        ry = area(ay, ay + wy, iy, iy + 1)
        for ix in range(math.floor(ax), math.ceil(ax + wx)):
            rx = area(ax, ax + wx, ix, ix + 1)
            val += rx * ry * sample(ix, iy)
    return val / (wx * wy)


def pair(a, length, i):
    l = area(a, a + length, i, i + 1)
    r = area(a, a + length, i + 1, i + 2)
    w = l + r
    return i + 0.5 + r / max(w, 1e-8), w


def linear(x, y, border_mode='edge'):
    x -= .5
    y -= .5
    i = math.floor(x)
    j = math.floor(y)
    fx = x - i
    fy = y - j
    if border_mode == 'border':
        p = lambda xx, yy: SRC[yy, xx] if 0 <= xx < W and 0 <= yy < H else 0.0
    else:
        p = sample
    return ((1 - fx) * (1 - fy) * p(i, j) + fx * (1 - fy) * p(i + 1, j) +
            (1 - fx) * fy * p(i, j + 1) + fx * fy * p(i + 1, j + 1))


def optimized(ax, ay, wx, wy, border_mode='edge'):
    nx = math.ceil(ax + wx) - math.floor(ax)
    ny = math.ceil(ay + wy) - math.floor(ay)
    ix = math.floor(ax)
    iy = math.floor(ay)
    v = 0.0
    for j in range(0, ny, 2):
        cy, ry = pair(ay, wy, iy + j)
        for i in range(0, nx, 2):
            cx, rx = pair(ax, wx, ix + i)
            v += rx * ry * linear(cx, cy, border_mode)
    return v / (wx * wy), math.ceil(nx/2) * math.ceil(ny/2), nx * ny


err_max = 0.0
fetches = defaultdict(list)
border_regression_count = 0
checks = 0
for ratio in [0.5, 1., 1.25, 4/3, 1.5, 1.75, 2., 2.5, 3., 4., 8.]:
    for _ in range(1600):
        ax = R.uniform(-.95, 29.2)
        ay = R.uniform(-.95, 27.2)
        wx = ratio
        wy = ratio * R.uniform(.6, 1.4)
        ref = reference(ax, ay, wx, wy)
        fast, nfast, nref = optimized(ax, ay, wx, wy)
        diff = abs(ref - fast)
        assert diff < 2.e-12, (ratio, ax, ay, diff)
        err_max = max(err_max, diff)
        fetches[ratio].append((nfast, nref))
        if ax < 0 or ay < 0 or ax + wx > W or ay + wy > H:
            wrong, _, _ = optimized(ax, ay, wx, wy, 'border')
            border_regression_count += abs(ref - wrong) > 0.001
        checks += 1

assert border_regression_count > 0
print(f'PASS: {checks} arbitrary-phase rectangles, no source/destination alignment assumed')
print(f'Max reference difference: {err_max:.3g}')
print(f'Vulkan border regression caught on {border_regression_count} out-of-bounds cases')
print('Scale  Avg Area taps  Avg Eden texels    Max taps')
for ratio, values in fetches.items():
    print(f'{ratio:5.3f} {np.mean([v[0] for v in values]):14.2f} {np.mean([v[1] for v in values]):17.2f} {max(v[0] for v in values):11d}')
