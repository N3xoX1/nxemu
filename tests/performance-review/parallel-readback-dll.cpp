// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise real Windows DLL thread detach, not just EXE-local destructors.
#include <memory>
#include <vector>
#include "yuzu_common/parallel_readback_copy.h"

extern "C" __declspec(dllexport) int RunReadback() try {
    constexpr size_t bytes=4*1024*1024+63;
    std::vector<unsigned char> source(bytes,0xC7), destination(bytes,0xC7);
    for(size_t i=0;i<bytes;i+=65536) destination[i]^=1;
#ifdef NXEMU_LEGACY_TLS
    // This deliberately reproduces the old DLL teardown bug in a disposable
    // child process. The runner verifies timeout and terminates only that child.
    thread_local std::unique_ptr<Common::ReadbackCopyWorkers> workers;
    if(!workers) workers=std::make_unique<Common::ReadbackCopyWorkers>(4);
    workers->Copy(destination.data(),source.data(),bytes);
#else
    if(Common::ReadbackCopyContext::Current()) return 2;
    {
        Common::ReadbackCopyContext context;
        bool parallel{};
        Common::CopyChangedMemoryForReadback(destination.data(),source.data(),bytes,&parallel);
        if(parallel!=(Common::ReadbackWorkerCount()!=0)) return 3;
        destination[2097151]^=1;
        Common::CopyChangedMemoryForReadback(destination.data(),source.data(),2097152,&parallel);
    }
    if(Common::ReadbackCopyContext::Current()) return 4;
#endif
    return source==destination ? 0 : 1;
} catch(...) { return 5; }
