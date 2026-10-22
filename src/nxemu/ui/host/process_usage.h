#pragma once
#include <cstdint>

// What the program takes of the computer, as the system counts it.

// The memory it holds, in bytes; 0 when the system does not say.
uint64_t ProcessMemory();
// The processor time all its threads have used since it started, in seconds.
double ProcessCpuSeconds();
