/* Test-only shim: Darwin has no tkill. The Darwin entry path uses __pthread_kill (328). */
#pragma once
#define __NR_tkill 0
