#pragma once
#include <cstdint>
#if !defined(_WIN32)
// Keep host libc declarations available to libstdc++/libc++ headers.
#include_next <unistd.h>
#endif
inline int32_t NxemuTestAndroidTid() { return 73; }
#define gettid NxemuTestAndroidTid
