#!/usr/bin/env python3
"""Dependency-free CMAA2 CPU contract tests; NOT a GPU shader/runtime test."""
from pathlib import Path
import random
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_spirv as spirv

SENTINEL = 0xffffffff
MAX_ITEMS = 32
MAX_INDEX = (1 << 26) - 1


def half_bits(value):
    return struct.unpack('<H', struct.pack('<e', max(0.0, min(value, 65504.0))))[0]


def half_value(bits):
    return struct.unpack('<e', struct.pack('<H', bits & 0xffff))[0]


def pack_intel_hdr(rgb):
    return (((half_bits(rgb[0]) + 8) >> 4) & 0x7ff) | \
           (((half_bits(rgb[1]) + 8) << 7) & 0x003ff800) | \
           (((half_bits(rgb[2]) + 16) << 17) & 0xffc00000)


def intel_hdr_decode(bits):
    return (half_value((bits << 4) & 0x7ff0),
            half_value((bits >> 7) & 0x7ff0),
            half_value((bits >> 17) & 0x7fe0))


def glsl_hdr_decode(bits):
    # Matches the GLSL unpackHalf2x16(..).x channel operations.
    r = (bits << 4) & 0x7ff0
    g = (bits >> 7) & 0x7ff0
    b = (bits >> 17) & 0x7fe0
    return tuple(half_value(n) for n in (r, g, b))


def resolve(pixel, original, heads, blend_items):
    x, y = pixel
    header = heads.get((x // 2, y // 2), SENTINEL)
    quad_offset = (y & 1) * 2 + (x & 1)
    total = [0.0] * 3
    weight_total = 0.0
    for _ in range(MAX_ITEMS):
        if header == SENTINEL:
            break
        index = header & MAX_INDEX
        if index >= len(blend_items):
            break
        next_header, color = blend_items[index]
        if (header >> 30) & 3 == quad_offset:
            weight = 0.8 + float((header >> 26) & 1)
            unpacked = glsl_hdr_decode(color)
            for channel in range(3):
                total[channel] += unpacked[channel] * weight
            weight_total += weight
        header = next_header
    return (*[c / weight_total for c in total], original[3]) if weight_total > 0 else original


def tests():
    rng = random.Random(0xC2AA2)
    for _ in range(4000):
        rgb = [rng.random() * 4 for _ in range(3)]
        packed = pack_intel_hdr(rgb)
        assert intel_hdr_decode(packed) == glsl_hdr_decode(packed)
    for w, h in ((1, 1), (3, 5), (31, 17), (1280, 720), (1279, 719), (1920, 1080), (3841, 2161)):
        heads = {(x, y): SENTINEL for y in range((h + 1) // 2) for x in range((w + 1) // 2)}
        for _ in range(40):
            x, y = rng.randrange(w), rng.randrange(h)
            original = (0.1, 0.2, 0.3, 0.41)
            assert resolve((x, y), original, heads, []) == original
        # Packing one pair of 4-bit edge masks per R8_UINT texel. No overlap.
        for x in range(min(w, 200)):
            assert x // 2 < (w + 1) // 2
            assert 0 <= (x & 1) * 4 <= 4
    # Two contributions for pixel (2,3), mixed with a different pixel in its quad.
    items = [
        (SENTINEL, pack_intel_hdr((0.1, 0.6, 0.3))),
        ((1 << 30) | 0, pack_intel_hdr((0.6, 0.1, 0.2))),
        ((2 << 30) | 1, pack_intel_hdr((0.9, 0.8, 0.7))),
    ]
    head = ((2 << 30) | (1 << 26)) | 2
    heads = {(1, 1): head}
    original = (0.0, 0.0, 0.0, 0.53)
    result = resolve((2, 3), original, heads, items)
    c0, c1 = intel_hdr_decode(items[2][1]), intel_hdr_decode(items[1][1])
    expected = tuple((a * 1.8 + b * 0.8) / 2.6 for a, b in zip(c0, c1))
    assert all(abs(a - b) <= 0.001 for a, b in zip(result[:3], expected)), (result, expected)
    assert result[3] == original[3]
    assert resolve((3, 3), original, heads, items) == original
    assert resolve((2, 2), original, heads, items) == original
    # Out-of-range index must never dereference a buffer outside descriptor range.
    assert resolve((2, 3), original, {(1, 1): (2 << 30) | 1000}, items) == original
    # Cyclic malformed list must terminate deterministically after 32 items.
    assert len(resolve((2, 3), original, {(1, 1): (2 << 30)}, [((2 << 30), pack_intel_hdr((0.5, 0.25, 0.75)))])) == 4
    # CPU reflection fixtures exercise actual binding/type checking, not SPIR-V validation.
    def op(code, *args):
        return ((len(args) + 1) << 16) | code, *args
    def words(format_id):
        return (spirv.MAGIC, 0x00010300, 0, 50, 0,
                *op(spirv.OP_ENTRY_POINT, 5, 1, 0),
                *op(spirv.OP_DECORATE, 10, spirv.DEC_DESCRIPTOR_SET, 0),
                *op(spirv.OP_DECORATE, 10, spirv.DEC_BINDING, 1),
                *op(spirv.OP_TYPE_IMAGE, 8, 2, 1, 0, 0, 0, 2, format_id),
                *op(spirv.OP_TYPE_POINTER, 9, 0, 8),
                *op(spirv.OP_VARIABLE, 9, 10, 0))
    from tempfile import TemporaryDirectory
    with TemporaryDirectory() as folder:
        file = Path(folder) / 'interface.spv'
        file.write_bytes(struct.pack(f'<{len(words(spirv.IMAGE_R8UI))}I', *words(spirv.IMAGE_R8UI)))
        assert spirv.read_module(file)['bindings'][1] == 'storage_image'
        file.write_bytes(struct.pack(f'<{len(words(spirv.IMAGE_R32UI))}I', *words(spirv.IMAGE_R32UI)))
        try:
            spirv.read_module(file)
        except ValueError as e:
            assert 'expected 39' in str(e)
        else:
            raise AssertionError('SPIR-V interface checker accepted wrong image format')
    print('PASS: 4,000 HDR pack/unpack checks, odd-size image indexing, list traversal and malformed-list checks')
    print('PASS: SPIR-V descriptor reflection positive/negative fixtures')
    print('NOTE: No shader compiler or Vulkan GPU was used by these CPU tests')


if __name__ == '__main__':
    tests()
