"""Compare one runtime Vulkan source/output capture to an independent box integral.
The input bytes are canonical RGBA8, source before presentation AA and output
before the watermark/UI. A meaningful comparison requires one opaque layer and
AA disabled; the metadata makes those assumptions visible.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def integral_resample(a, positions, axis):
    size = a.shape[axis]
    c = np.cumsum(a, axis=axis, dtype=np.float64)
    padshape = list(c.shape)
    padshape[axis] = 1
    c = np.concatenate([np.zeros(padshape), c], axis=axis)
    clipped = np.clip(positions, 0, size)
    index = np.minimum(np.floor(clipped).astype(int), size - 1)
    fraction = clipped - index
    shape = [1] * a.ndim
    shape[axis] = len(positions)
    values = np.take(c, index, axis=axis) + np.take(a, index, axis=axis) * fraction.reshape(shape)
    values += np.take(a, [0], axis=axis) * np.minimum(positions, 0).reshape(shape)
    values += np.take(a, [size - 1], axis=axis) * np.maximum(positions - size, 0).reshape(shape)
    return np.diff(values, axis=axis) / np.diff(positions).reshape(
        [len(positions)-1 if i == axis else 1 for i in range(a.ndim)])


def exact_area(source, crop, width, height):
    sh, sw = source.shape[:2]
    left, top, right, bottom = crop
    xs = np.linspace(left*sw, right*sw, width+1)
    ys = np.linspace(top*sh, bottom*sh, height+1)
    # Bound temporary memory for large source images.
    intermediate = np.empty((sh, width, 4), dtype=np.float64)
    for y in range(0, sh, 16):
        intermediate[y:y+16] = integral_resample(source[y:y+16], xs, 1)
    return integral_resample(intermediate, ys, 0)


def analyze(metadata):
    path = Path(metadata)
    info = json.loads(path.read_text(encoding="utf-8"))
    prefix = str(path)[:-5]
    source = np.fromfile(prefix+"-source.rgba", np.uint8).reshape(info["source_height"], info["source_width"], 4)
    output = np.fromfile(prefix+"-output.rgba", np.uint8).reshape(info["output_height"], info["output_width"], 4)
    left, top, right, bottom = info["screen"]
    target = output[top:bottom, left:right]
    reference = exact_area(source, info["crop"], right-left, bottom-top)
    diff = np.abs(reference-target)
    # Per-channel difference includes target UNORM8 rounding.
    stats = dict(info, rgba_max_error=float(diff.max()), rgba_mean_error=float(diff.mean()),
                 pixels_error_gt2_percent=float(100*np.mean(np.max(diff, axis=2)>2)),
                 source_shape_matches_reported=(info["source_width"]==info["reported_width"] and
                                                info["source_height"]==info["reported_height"]))
    Image.fromarray(source).save(prefix+"-source.png")
    Image.fromarray(output).save(prefix+"-output.png")
    Image.fromarray(target).save(prefix+"-display.png")
    Image.fromarray(np.uint8(np.clip(np.round(reference),0,255))).save(prefix+"-cpu-area.png")
    Path(prefix+"-analysis.json").write_text(json.dumps(stats,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(stats,indent=2))
    return stats


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata")
    analyze(parser.parse_args().metadata)
