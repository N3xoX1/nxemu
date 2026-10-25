/* Test-only shim: arm_nce_asm_definitions.h includes Linux headers. On Darwin the
   signal numbers come from <signal.h> (fixed values for the assembler). A real port
   replaces these includes. */
#pragma once
#ifdef __ASSEMBLER__
#define SIGBUS 10
#define SIGSEGV 11
#define SIGURG 16
#define SIGUSR2 31
#else
#include <signal.h>
#endif
