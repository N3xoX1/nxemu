// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "nxemu-module-spec/storage_read_traffic.h"

namespace FileSys
{

// Bytes returned by native backing-file reads on this thread. The OS module
// samples the counter around synchronous filesystem reads. Internal VFS cache
// hits and memory files never reach RealVfsFile and add no backing traffic.
inline thread_local StorageReadTraffic backing_read_traffic;

} // namespace FileSys
