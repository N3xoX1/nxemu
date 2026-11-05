#!/usr/bin/env python3
"""Reflect compiled SPIR-V and fail on CMAA2 Vulkan descriptor/interface drift.

This is *not* a substitute for spirv-val or validation layers. Only use on
SPIR-V binaries that were successfully compiled and passed spirv-val.
"""
import argparse
from pathlib import Path
import struct

MAGIC = 0x07230203
OP_ENTRY_POINT = 15
OP_DECORATE = 71
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_POINTER = 32
OP_VARIABLE = 59
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
STORAGE_UNIFORM_CONSTANT = 0
STORAGE_UNIFORM = 2
STORAGE_STORAGE_BUFFER = 12
STORAGE_IMAGE = 0
IMAGE_R32UI = 33
IMAGE_R8UI = 39

# Each set/binding is the Vulkan descriptor layout in IntelCMAA2::CreateLayout.
# The HLSL DXC -fvk-*-shift values place Intel's u/t/s registers here.
LAYOUT = {
    1: 'storage_image',
    2: 'storage_buffer',
    3: 'storage_buffer',
    4: 'storage_buffer',
    5: 'storage_image',
    6: 'storage_buffer',
    7: 'storage_buffer',
    8: 'sampled_image',
    12: 'sampler',
}
COMPUTE_ONLY = {1, 2, 3, 6, 7}
FRAGMENT_ALLOWED = {4, 5, 8, 12}


def read_module(path: Path) -> dict:
    content = path.read_bytes()
    if len(content) < 20 or len(content) % 4:
        raise ValueError(f'{path}: invalid SPIR-V byte length')
    words = struct.unpack(f'<{len(content)//4}I', content)
    if words[0] != MAGIC:
        raise ValueError(f'{path}: invalid SPIR-V magic')
    defs = {}
    decorated = {}
    pointers = {}
    variables = {}
    entry_models = set()
    i = 5
    while i < len(words):
        opcode_word = words[i]
        n, op = opcode_word >> 16, opcode_word & 0xffff
        if not n or i + n > len(words):
            raise ValueError(f'{path}: malformed instruction at word {i}')
        a = words[i+1:i+n]
        if op == OP_DECORATE and len(a) >= 3 and a[1] in (DEC_BINDING, DEC_DESCRIPTOR_SET):
            decorated.setdefault(a[0], {})[a[1]] = a[2]
        elif op == OP_TYPE_IMAGE and len(a) >= 8:
            # OpTypeImage ResultId SampledType Dim Depth Arrayed MS Sampled Format ...
            defs[a[0]] = ('image', a[6], a[7])
        elif op == OP_TYPE_SAMPLER:
            defs[a[0]] = ('sampler',)
        elif op == OP_TYPE_SAMPLED_IMAGE:
            defs[a[0]] = ('sampled_image', a[1])
        elif op == OP_TYPE_POINTER:
            pointers[a[0]] = (a[1], a[2])
        elif op == OP_VARIABLE:
            variables[a[1]] = (a[0], a[2])
        elif op == OP_ENTRY_POINT:
            entry_models.add(a[0])
        i += n
    bindings = {}
    for identifier, decorators in decorated.items():
        if DEC_BINDING not in decorators:
            continue
        if DEC_DESCRIPTOR_SET not in decorators:
            raise ValueError(f'{path}: binding {decorators[DEC_BINDING]} without descriptor set')
        set_id = decorators[DEC_DESCRIPTOR_SET]
        binding = decorators[DEC_BINDING]
        if set_id != 0 or binding not in LAYOUT:
            raise ValueError(f'{path}: unexpected Vulkan descriptor set={set_id}, binding={binding}')
        if identifier not in variables:
            continue  # decorations can also apply to non-variable IDs
        pointer_type, storage = variables[identifier]
        if pointer_type not in pointers:
            raise ValueError(f'{path}: missing pointer type for descriptor {binding}')
        _, pointee_type = pointers[pointer_type]
        actual = defs.get(pointee_type, ('buffer',))
        if storage == STORAGE_STORAGE_BUFFER or storage == STORAGE_UNIFORM:
            kind = 'storage_buffer'
        elif actual[0] == 'sampler':
            kind = 'sampler'
        elif actual[0] == 'sampled_image':
            kind = 'sampled_image'
        elif actual[0] == 'image':
            # OpTypeImage Sampled=2 means storage image. Sampled=1/0 means sampled.
            kind = 'storage_image' if actual[1] == 2 else 'sampled_image'
            if kind == 'storage_image' and binding in (1, 5):
                required_format = IMAGE_R8UI if binding == 1 else IMAGE_R32UI
                if actual[2] != required_format:
                    raise ValueError(f'{path}: binding {binding} uses SPIR-V ImageFormat {actual[2]}, expected {required_format}')
        else:
            raise ValueError(f'{path}: unsupported descriptor type at binding {binding}: {actual}')
        if kind != LAYOUT[binding]:
            raise ValueError(f'{path}: binding {binding} is {kind}, Vulkan C++ expects {LAYOUT[binding]}')
        if binding in bindings:
            raise ValueError(f'{path}: duplicated descriptor binding {binding}')
        bindings[binding] = kind
    return {'bindings': bindings, 'models': entry_models}


def check(stage: str, path: Path):
    module = read_module(path)
    expected_model = 4 if stage == 'frag' else 0 if stage == 'vert' else 5
    if expected_model not in module['models']:
        raise ValueError(f'{path}: expected execution model {expected_model}, saw {module["models"]}')
    bindings = set(module['bindings'])
    allowed = FRAGMENT_ALLOWED if stage == 'frag' else set() if stage == 'vert' else set(LAYOUT)
    if not bindings <= allowed:
        raise ValueError(f'{path}: illegal {stage} descriptor bindings: {bindings - allowed}')
    if stage == 'frag' and not {4, 5, 8} <= bindings:
        raise ValueError(f'{path}: fragment resolve missing essential bindings: { {4, 5, 8} - bindings }')
    if stage in ('edges', 'args', 'candidates') and 0 in bindings:
        raise ValueError(f'{path}: compute shader unexpectedly uses removed storage output u0')
    print(f'PASS {stage}: descriptors {sorted(bindings)}, execution model {expected_model}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--edges', type=Path, required=True)
    ap.add_argument('--args', type=Path, required=True)
    ap.add_argument('--candidates', type=Path, required=True)
    ap.add_argument('--vert', type=Path, required=True)
    ap.add_argument('--frag', type=Path, required=True)
    arguments = vars(ap.parse_args())
    for stage, path in arguments.items():
        check(stage, path)
    print('PASS: CMAA2 cross-stage SPIR-V Vulkan interface verification')


if __name__ == '__main__':
    main()
