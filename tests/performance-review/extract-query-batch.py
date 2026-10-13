"""Extract the production query sequence and scheduler chunk, plus the committed oracle."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'build/performance-live-20261011/query-batch-tests'
out.mkdir(parents=True, exist_ok=True)
path = 'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp'
current = (root / path).read_text()
baseline = subprocess.check_output(['git', 'show', f'5166b027:{path}'], cwd=root, text=True)

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

helper = function(current, 'void QueryCopyBarrier(')
old_barriers = function(baseline, 'void QueryCacheRuntime::Barriers(')
# The exact two barrier initializers and stage masks must remain unchanged.
for name in ('READ_BARRIER', 'WRITE_BARRIER'):
    def initializer(text):
        start = text.index('static constexpr VkMemoryBarrier ' + name)
        return ''.join(text[start:text.index('};', start) + 2].split())
    assert initializer(helper) == initializer(old_barriers)
assert helper.count('VK_PIPELINE_STAGE_ALL_COMMANDS_BIT') == 2
assert helper.count('VK_PIPELINE_STAGE_TRANSFER_BIT') == 2
(out / 'query-barrier.inc').write_text(helper)
manifest = {'barrier': hashlib.sha256(helper.encode()).hexdigest()}
for mode, text in (('old', baseline), ('new', current)):
    sync = function(text, 'void QueryCacheRuntime::SyncValues(')
    prefix, tail = sync.split('    for (const auto& [buffer, offset] : impl->buffers_to_upload_to)', 1)
    if mode == 'old':
        old_prefix = prefix
    else:
        assert prefix == old_prefix, 'Buffer acquisition and copy regions changed unexpectedly'
    sequence = '    for (const auto& [buffer, offset] : impl->buffers_to_upload_to)' + tail
    (out / f'query-{mode}.inc').write_text(
        f'void QueryHarness::{mode.title()}Sync(VkBuffer src_buffer) {{\n' + sequence)
    manifest[mode] = hashlib.sha256(sync.encode()).hexdigest()
barriers = function(current, 'void QueryCacheRuntime::Barriers(').replace('QueryCacheRuntime::', 'QueryHarness::')
(out / 'query-generic-barriers.inc').write_text(barriers)
header = (root / 'src/yuzu_video_core/renderer_vulkan/vk_scheduler.h').read_text()
classes = header[header.index('    class Command {'):header.index('    struct State {')]
(out / 'command-chunk.inc').write_text(classes.replace('private:', 'public:'))
scheduler = (root / 'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp').read_text()
execute = function(scheduler, 'void Scheduler::CommandChunk::ExecuteAll(').replace('Scheduler::', 'ChunkHarness::')
(out / 'command-execute.inc').write_text(execute)
(out / 'extraction.json').write_text(json.dumps(manifest, indent=2))
print('PASS exact barriers, unchanged copy setup and actual scheduler chunk extracted')
