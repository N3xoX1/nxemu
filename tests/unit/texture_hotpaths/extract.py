"""Extract production hot paths; fixtures replace only resource/backend dependencies."""
from pathlib import Path
import sys
root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)
def method(s, signature):
    start = s.index(signature)
    brace = s.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
s = (root / 'src/yuzu_video_core/texture_cache/texture_cache.h').read_text()
names = ['ForEachImageInRegion', 'ForEachImageInRegionGPU']
(output / 'regions.inc').write_text('\n\n'.join('template<typename Func>\n' + method(s, 'void TextureCache<P>::' + n + '(').replace('TextureCache<P>::', 'Region::') for n in names))
s = (root / 'src/yuzu_video_core/texture_cache/texture_cache_base.h').read_text()
(output / 'pages.inc').write_text('\n\n'.join('template<typename Func>\n' + method(s, 'static void ' + n + '(') for n in ['ForEachCPUPage', 'ForEachGPUPage']))
