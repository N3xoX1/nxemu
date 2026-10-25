/* Linked with -pagezero_size 0x4000: checks whether the Darwin kernel runs an arm64
   executable whose __PAGEZERO is smaller than 4 GiB, and whether low guest addresses
   (128 MiB, the start of the 39-bit Horizon map region) can then be mapped. */
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    void *p = mmap((void *)0x8000000, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    printf("pagezero_probe: running, map 128MiB %s\n", p == MAP_FAILED ? "failed" : "ok");
    return 0;
}
