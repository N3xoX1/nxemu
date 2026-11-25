"""Compile production command processing bodies against deterministic host fixtures."""
from pathlib import Path
import sys
root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)
def method(source, name):
    anchor = source.index(name + '(')
    start = source.rfind('\n', 0, anchor) + 1
    brace = source.index('{', anchor)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
h = (root / 'src/yuzu_video_core/engines/maxwell_3d.h').read_text()
kinds = h.index('enum StreamKind')
(output / 'stream.inc').write_text(h[h.rfind('\n', 0, kinds) + 1:h.index('};', kinds) + 2])
s = (root / 'src/yuzu_video_core/engines/maxwell_3d.cpp').read_text()
names = ['IsMethodExecutable', 'ProcessShadowRam', 'ProcessDirtyRegisters',
         'PushMethod', 'PushMethodRun', 'ConsumeSinkImpl', 'CallMethod', 'ProcessMethodCall',
         'FrontMethodCall', 'RunWords', 'FrontHLEMacro']
engine = [method(s, 'Maxwell3D::'+n).replace('Maxwell3D::','Fixture::') for n in names]
engine += [method(h, 'void ' + n).replace('void '+n+'(', 'void Fixture::'+n+'(', 1) for n in ['FrontWrite', 'StreamHeader']]
(output / 'engine.inc').write_text('\n\n'.join(engine))
(output / 'writes.inc').write_text('\n\n'.join(
    method(s, 'Maxwell3D::' + n).replace('Maxwell3D::', 'WriteRanges::') for n in
    ['UnpublishedMayWrite', 'ConvertWriteRanges', 'ClearWriteRanges', 'FrontMemoryWrite']))
names = ['SinkFor', 'ProcessCommands', 'SetState', 'CallMethod', 'CallMultiMethod',
         'UpdateUniformPrefetchAtAcquire', 'ResumeUniformPrefetch', 'PullerWrites',
         'EngineWrites', 'UploadWrites', 'CopyWrites', 'BlitWrites', 'BlockLinearSize',
         'TrackFrontRegister', 'ApplyFrontSink']
s = (root / 'src/yuzu_video_core/dma_pusher.cpp').read_text()
operation = s.index('enum class SemaphoreOperation')
upload = s.index('// KeplerCompute and KeplerMemory share')
dma = [s[upload:s.index('\n\n', upload)], method(s, 'IsPullerRegisterOnly'),
       s[operation:s.index('};', operation) + 2], method(s, 'IsPullerAcquire')]
dma += [method(s, 'DmaPusher::'+n).replace('DmaPusher::','Dma::') for n in names]
(output / 'dma.inc').write_text('\n\n'.join(dma))
(output / 'probe.inc').write_text('\n\n'.join(
    method(s, 'DmaPusher::' + n).replace('DmaPusher::', 'Probe::') for n in
    ['FetchesNewPages', 'IsKnownPushbuffer', 'DispatchMayWrite']))
(output / 'baseline.inc').write_text((root / 'tests/unit/gpu_commands/baseline.inc').read_text())

h = (root / 'src/yuzu_video_core/gpu_thread.h').read_text()
start = h.index('class BackQueue final')
queue = h[start:h.index('\n};', start) + 3].replace('private:', 'public:')
s = (root / 'src/yuzu_video_core/gpu_thread.cpp').read_text()
limit = s.index('constexpr size_t MaxWriteRanges')
queue = s[limit:s.index('\n', limit) + 1] + queue
queue += '\n' + '\n'.join(method(s, 'BackQueue::' + n) for n in
    ['Push', 'WaitIdle', 'AddWrites', 'MayWrite', 'MayWriteAny', 'DropFinishedWrites', 'ClearWrites',
     'UpdateWriteBounds', 'PopWait',
     'MarkRun', 'FinishMany', 'Notify', 'Stop'])
(output / 'queue.inc').write_text(queue)
(output / 'stop.inc').write_text(method(s, 'ThreadManager::StopThread'))
