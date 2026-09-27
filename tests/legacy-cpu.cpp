// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-model enforcement control only; never opens the decoder or firmware.
#include <cpuid.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--sse")) {
        __asm__ volatile("xorps %xmm0, %xmm0");
        return 1; // A no-SSE CPU must instead terminate with SIGILL.
    }
    if (argc == 2 && !std::strcmp(argv[1], "--sse2")) {
        __asm__ volatile("pxor %xmm0, %xmm0");
        return 1;
    }
    if (argc != 1) return 2;
    unsigned a, b, c, d;
    if (sizeof(void *) != 4 || !__get_cpuid(1, &a, &b, &c, &d) ||
        !(d & bit_CMOV) || (d & (bit_SSE | bit_SSE2))) {
        std::fprintf(stderr, "expected a 32-bit i686 CPU without SSE/SSE2\n");
        return 1;
    }
    std::puts("CPU model: i686, SSE=0, SSE2=0");
    return 0;
}
