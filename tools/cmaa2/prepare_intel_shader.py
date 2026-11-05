#!/usr/bin/env python3
"""Adapt pinned Intel CMAA2 for Vulkan DXC and bounds-safe scratch-buffer writes.

The original source is Apache-2.0, copyright (c) 2018 Intel Corporation.
Modifications: Vulkan typed-storage formats (R8UI edges/R32UI heads),
full-quad linked-list initialization for fragment resolve, and scratch-list bounds checks.
Intel's shape and blend computations remain unchanged.
"""
import hashlib
from pathlib import Path
import sys
EXPECTED='717101c85ef6e933dee3d193f514410b51d09c45'

if len(sys.argv) != 3:
    raise SystemExit('Usage: prepare_intel_shader.py PINNED_SOURCE.HLSL OUTPUT.HLSL')
src=Path(sys.argv[1]); out=Path(sys.argv[2]); b=src.read_bytes()
actual=hashlib.sha1(f'blob {len(b)}\0'.encode()+b).hexdigest()
if actual!=EXPECTED: raise SystemExit(f'Unknown Intel CMAA2 revision {actual}; expected {EXPECTED}')
s=b.decode('utf-8')
# Vulkan image-fetch offsets must be constants. Fold the dynamic offset into
# the signed coordinate instead. Clamp source colors at the visible border so
# partial tiles do not create artificial black edges (especially on 1x1 images).
load = 'g_inoutColorReadonly.Load( int3( pixelPos, 0 ), offset ).rgb'
assert s.count(load) == 1
s = s.replace(load, 'LoadSourceColorVulkan(int2(pixelPos) + offset)')
helper = '''lpfloat3 LoadSourceColorVulkan(int2 pixel)
{
    uint width, height;
    g_inoutColorReadonly.GetDimensions(width, height);
    return g_inoutColorReadonly.Load(int3(clamp(pixel, int2(0, 0), int2(width, height) - 1), 0)).rgb;
}
'''
s = s.replace('lpfloat3 LoadSourceColor( uint2 pixelPos', helper + '\nlpfloat3 LoadSourceColor( uint2 pixelPos', 1)
needle='RWTexture2D<lpfloat4>           g_inoutColorWriteonly'
assert s.count(needle)==1, 'Upstream declaration changed'
s=s.replace(needle, '[[vk::image_format("rgba16f")]] '+needle)
for declaration, vk_format in [
    ('RWTexture2D<uint>               g_workingEdges', 'r8ui'),
    ('RWTexture2D<uint>               g_workingDeferredBlendItemListHeads', 'r32ui'),
]:
    if s.count(declaration) != 1:
        raise SystemExit('Intel typed scratch image declaration changed: ' + declaration)
    s = s.replace(declaration, f'[[vk::image_format("{vk_format}")]] ' + declaration)
# Fragment output covers every quad. Intel only clears heads on edge-containing quads;
# stale untouched heads would otherwise be read by the new fullscreen apply shader.
head_clear = '''                // Clear deferred color list heads to empty (if potentially needed - even though some edges might get culled by local contrast adaptation\x20
                // step below, it's still cheaper to just clear it without additional logic)
                g_workingDeferredBlendItemListHeads[ uint2( pixelPos ) / 2 ] = 0xFFFFFFFF;
'''
assert s.count(head_clear) == 1, 'Upstream head clear changed'
s=s.replace(head_clear, '')
marker='''            [branch]
            if( someNonZeroEdges )'''
assert s.count(marker) == 1
s=s.replace(marker,'''            // The fullscreen apply shader reads every quad, even if it has no edges.
            // Initialise all heads before the candidate pass inserts any contributions.
            g_workingDeferredBlendItemListHeads[ uint2( pixelPos ) / 2 ] = 0xFFFFFFFF;
            [branch]
            if( someNonZeroEdges )''')
# The earlier head clear was conditional on MSAA=1; keep corresponding preprocessor
# structure syntactically valid without an empty block.
s=s.replace('''    #if CMAA_MSAA_SAMPLE_COUNT == 1
    #endif
''', '')

alpha_needle = 'g_inoutColorWriteonly[ pixelPos ] = lpfloat4( color.rgb, 0 );'
if s.count(alpha_needle) != 1:
    raise SystemExit('Upstream Intel alpha output code changed')
s=s.replace(alpha_needle,
    'g_inoutColorWriteonly[ pixelPos ] = lpfloat4( color.rgb, g_inoutColorReadonly.Load( int3( pixelPos, 0 ) ).a );')
# Intel's space-saving buffer allocation is explicitly allowed to overflow on very
# noisy images. A Vulkan OOB write must not be allowed to corrupt adjacent GPU data.
# Counters can exceed capacity; Arguments clamps candidate counts before indirect dispatch.
# Dropped items leave their original source pixels unchanged by the final fragment pass.
checks = [
    (
        'g_workingShapeCandidates[counterIndex] = (localPixelPos.x << 18) | (msaaSampleIndex << 14) | localPixelPos.y;',
        """uint shapeCount, shapeStride;
                        g_workingShapeCandidates.GetDimensions(shapeCount, shapeStride);
                        if (counterIndex < shapeCount)
                            g_workingShapeCandidates[counterIndex] = (localPixelPos.x << 18) | (msaaSampleIndex << 14) | localPixelPos.y;""",
    ),
    (
        'uint2 quadPos       = pixelPos / uint2( 2, 2 );',
        """uint itemCount, itemStride;
    g_workingDeferredBlendItemList.GetDimensions(itemCount, itemStride);
    // The item index is stored in only 26 bits of the linked-list header.
    if (counterIndex >= itemCount || counterIndex >= (1u << 26))
        return;
    uint2 quadPos       = pixelPos / uint2( 2, 2 );""",
    ),
    (
        'g_workingDeferredBlendLocationList[edgeListCounter] = (quadPos.x << 16) | quadPos.y;',
        """uint locationCount, locationStride;
        g_workingDeferredBlendLocationList.GetDimensions(locationCount, locationStride);
        if (edgeListCounter < locationCount)
            g_workingDeferredBlendLocationList[edgeListCounter] = (quadPos.x << 16) | quadPos.y;""",
    ),
]
for original, replacement in checks:
    if s.count(original) != 1:
        raise SystemExit('Intel scratch-buffer write changed: ' + original)
    s = s.replace(original, replacement)
# Explicit bounds also cover partial 2x2 quads and padded 28x28 dispatch tiles.
# Group barriers remain unconditional; only global image operations are guarded.
def replace_once(original, replacement):
    if s.count(original) != 1:
        raise SystemExit('Intel port substitution changed: ' + original)
    return s.replace(original, replacement)

s = replace_once('if( isCandidate )', '''uint imageWidth, imageHeight;
                    g_inoutColorReadonly.GetDimensions(imageWidth, imageHeight);
                    if( isCandidate && all(localPixelPos < uint2(imageWidth, imageHeight)) )''')
s = replace_once('''void StoreColorSample( uint2 pixelPos, lpfloat3 color, bool isComplexShape, uint msaaSampleIndex )
{''', '''void StoreColorSample( uint2 pixelPos, lpfloat3 color, bool isComplexShape, uint msaaSampleIndex )
{
    uint width, height;
    g_inoutColorReadonly.GetDimensions(width, height);
    if (any(pixelPos >= uint2(width, height))) return;''')
s = replace_once('''uint LoadEdge( int2 pixelPos, int2 offset, uint msaaSampleIndex )
{''', '''uint LoadEdge( int2 pixelPos, int2 offset, uint msaaSampleIndex )
{
    uint width, height;
    g_inoutColorReadonly.GetDimensions(width, height);
    int2 coordinate = pixelPos + offset;
    if (any(coordinate < 0) || any(coordinate >= int2(width, height))) return 0;''')
s = replace_once('''            // The fullscreen apply shader reads every quad, even if it has no edges.
            // Initialise all heads before the candidate pass inserts any contributions.
            g_workingDeferredBlendItemListHeads[ uint2( pixelPos ) / 2 ] = 0xFFFFFFFF;''', '''            // The fullscreen apply shader reads every quad, even if it has no edges.
            uint imageWidth, imageHeight;
            g_inoutColorReadonly.GetDimensions(imageWidth, imageHeight);
            if (all(pixelPos < uint2(imageWidth, imageHeight)))
                g_workingDeferredBlendItemListHeads[ uint2( pixelPos ) / 2 ] = 0xFFFFFFFF;''')
for row in (0, 1):
    original = f'g_workingEdges[ int2(pixelPos.x/2, pixelPos.y+{row}) ] = (outEdges[{row*2+1}] << 4) | outEdges[{row*2}];'
    s = replace_once(original, f'''uint edgeWidth{row}, edgeHeight{row};
        g_workingEdges.GetDimensions(edgeWidth{row}, edgeHeight{row});
        if (pixelPos.x / 2 < edgeWidth{row} && pixelPos.y + {row} < edgeHeight{row})
            {original}''')

# Bound contributions by the allocated scratch images too: accelerated source
# textures may include padding beyond the logical presentation extent.
needle = 'if (any(coordinate < 0) || any(coordinate >= int2(width, height))) return 0;'
s = replace_once(needle, '''uint edgeWidth, edgeHeight;
    g_workingEdges.GetDimensions(edgeWidth, edgeHeight);
    if (any(coordinate < 0) || any(coordinate >= int2(min(uint2(width, height), uint2(edgeWidth * 2, edgeHeight))))) return 0;''')
needle = 'if (any(pixelPos >= uint2(width, height))) return;'
s = replace_once(needle, '''uint edgeWidth, edgeHeight;
    g_workingEdges.GetDimensions(edgeWidth, edgeHeight);
    if (any(pixelPos >= min(uint2(width, height), uint2(edgeWidth * 2, edgeHeight)))) return;''')
needle = 'if( isCandidate && all(localPixelPos < uint2(imageWidth, imageHeight)) )'
s = replace_once(needle, '''uint edgeWidth, edgeHeight;
                    g_workingEdges.GetDimensions(edgeWidth, edgeHeight);
                    if( isCandidate && all(localPixelPos < min(uint2(imageWidth, imageHeight), uint2(edgeWidth * 2, edgeHeight))) )''')
needle = 'if (all(pixelPos < uint2(imageWidth, imageHeight)))'
s = replace_once(needle, '''uint headWidth, headHeight;
            g_workingDeferredBlendItemListHeads.GetDimensions(headWidth, headHeight);
            if (all(pixelPos / 2 < uint2(headWidth, headHeight)))''')

# Fullscreen resolve never consumes Intel's compact location list. Remove its
# atomic append and reset counters while producing candidate dispatch arguments.
# This saves a compute dispatch, a compute barrier and the per-quad location append.
start = s.index('    // First one added?')
end = s.index('\n}', start)
s = s[:start] + s[end:]
needle = 'g_workingControlBuffer.Store( 4*3, shapeCandidateCount );'
s = replace_once(needle, needle + '''
        g_workingControlBuffer.Store(4*4, 0);
        g_workingControlBuffer.Store(4*12, 0);''')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(s)
