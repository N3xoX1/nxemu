"""Inspect a diagnostic IR dump; do not modify guest shaders or game files."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import struct


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def analyze(path, scale):
    instructions = {}
    consumers = defaultdict(list)
    for line in path.read_text().splitlines():
        match = re.search(r"\]\s+%(\d+)\s*=\s*(\w+)(.*?)\s*\(uses:", line)
        if not match:
            continue
        number, opcode, arguments = match.groups()
        number = int(number)
        refs = [int(ref) for ref in re.findall(r"%(\d+)", arguments)]
        instructions[number] = (opcode, arguments.strip(), refs)
        for ref in refs:
            consumers[ref].append(number)

    roots = [n for n, (op, args, _) in instructions.items()
             if op == "GetAttribute" and args.startswith("Position.X")]
    if len(roots) != 1:
        raise ValueError("Expected exactly one horizontal fragment-coordinate root")

    # Walk every SSA use, stopping at quantization or its finite-input NaN check.
    # A texture/output/control sink reached before either would invalidate this proof.
    frontier = set()
    visited = set()
    pending = roots[:]
    escaped = []
    while pending:
        number = pending.pop()
        if number in visited:
            continue
        visited.add(number)
        opcode, _, _ = instructions[number]
        if opcode in ("FPTrunc32", "FPIsNan32"):
            frontier.add(number)
            continue
        if not consumers[number] or opcode not in (
                "GetAttribute", "ResolutionDownFactor", "Identity", "FPMul32"):
            escaped.append(number)
        pending.extend(consumers[number])
    if escaped:
        raise ValueError(f"Unquantized X paths need manual review: {escaped}")
    truncations = sorted(n for n in frontier if instructions[n][0] == "FPTrunc32")
    if len(truncations) != 2:
        raise ValueError("Expected the two observed native-X and half-X truncations")

    def evaluate(number, host_x):
        opcode, args, refs = instructions[number]
        values = [evaluate(ref, host_x) for ref in refs]
        if opcode == "GetAttribute":
            if not args.startswith("Position.X"):
                raise ValueError(args)
            return f32(host_x)
        if opcode == "ResolutionDownFactor":
            return f32(1 / scale)
        if opcode == "Identity":
            return values[0]
        if opcode == "FPMul32":
            if len(values) == 1:
                values.append(float(re.search(r"#([^, ]+)", args)[1]))
            return f32(values[0] * values[1])
        if opcode == "FPTrunc32":
            return int(values[0])
        raise ValueError(f"Unsupported coordinate opcode {opcode}")

    # Entire useful portable width: every group must share both quantized inputs.
    width = 1280 * scale
    pairs = [tuple(evaluate(n, x + 0.5) for n in truncations) for x in range(width)]
    same_groups = all(len(set(pairs[x:x + scale])) == 1
                      for x in range(0, width, scale))
    return {
        "file": str(path), "internal_scale": scale, "physical_width": width,
        "x_root": roots[0],
        "frontier": [{"ssa": n, "opcode": instructions[n][0]} for n in sorted(frontier)],
        "unquantized_x_escape": escaped,
        "all_native_pixel_groups_have_identical_quantized_x": same_groups,
        "unique_coordinate_pairs": len(set(pairs)),
        "first_12_pixels": [
            {"physical_x": x, "guest_integer_x": pair[0],
             "guest_half_x": pair[1], "column_parity": pair[0] & 1}
            for x, pair in enumerate(pairs[:12])],
        "limits": "Checks coordinate dependency and quantization, not texture contents, "
                  "implicit LOD derivatives or a proposed shader correction.",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("--scale", type=int, default=3, choices=(1, 2, 3, 4, 5, 6))
    args = parser.parse_args()
    print(json.dumps(analyze(args.ir, args.scale), indent=2))
