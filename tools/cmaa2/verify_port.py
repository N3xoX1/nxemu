#!/usr/bin/env python3
"""Verify the real pinned-source transform; compile/GPU checks run separately."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'src/3rd_party/intel_cmaa2/CMAA2.hlsl'
PREPARE = ROOT / 'tools/cmaa2/prepare_intel_shader.py'
with tempfile.TemporaryDirectory(prefix='cmaa2-transform-') as folder:
    folder = Path(folder)
    output = folder / 'prepared.hlsl'
    subprocess.run([sys.executable, str(PREPARE), str(SOURCE), str(output)], check=True)
    text = output.read_text()
    for required in ('LoadSourceColorVulkan(int2(pixelPos) + offset)',
                     'counterIndex < shapeCount', 'counterIndex >= itemCount',
                     'g_workingControlBuffer.Store(4*4, 0)',
                     'g_workingControlBuffer.Store(4*12, 0)',
                     'localPixelPos < min(uint2(imageWidth, imageHeight), uint2(edgeWidth * 2, edgeHeight))'):
        assert required in text, required
    store = text[text.index('void StoreColorSample('):text.index('#define CMAA2_BLEND_ITEM_SLM_SIZE')]
    assert 'g_workingDeferredBlendLocationList[' not in store
    assert 'InterlockedAdd( 4*8' not in store
    changed = folder / 'changed.hlsl'
    changed.write_bytes(SOURCE.read_bytes() + b'\n')
    result = subprocess.run([sys.executable, str(PREPARE), str(changed), str(output)], capture_output=True)
    assert result.returncode != 0, 'Changed upstream input was accepted'
print('PASS: pinned Intel source transform, bounds checks, counter reset and tamper rejection')
