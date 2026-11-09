#pragma once
#include "core/file_sys/vfs/vfs.h"
#include "nxformat/nx_session.hpp"

namespace FileSys {
// Every directory/file keeps the mounted session alive, including RomFS views
// retained by the OS after the application loader has released its own handle.
VirtualFile NxSourceResourceFile(std::shared_ptr<nx::Session> session, uint32_t resource, std::string_view name);
VirtualDir NxSnapshotDirectory(std::shared_ptr<nx::Session> session, uint32_t snapshot);
nx::Result<std::shared_ptr<nx::Session>> OpenNxSession(VirtualFile file,
    std::string_view host_path = {}, nx::SessionOptions options = {});
struct NxRuntimeStats {
    uint64_t source_read_calls=0, source_read_bytes=0;
    uint64_t decoder_bytes=0, decoder_peak_bytes=0, decoder_contexts=0, decoder_pool_bytes=0;
};
// Only for sessions returned by OpenNxSession; values exclude allocator overhead.
NxRuntimeStats NxSessionStatistics(const nx::Session& session);
}
