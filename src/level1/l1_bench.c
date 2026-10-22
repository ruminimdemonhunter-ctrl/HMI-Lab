#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    const uint64_t n = 10000000;
    L1Core c; l1_init(&c);
    L1Observation o = {3400, 0, 0, true};
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    volatile unsigned checksum = 0;
    for (uint64_t i = 0; i < n; i++) {
        o.cpu_some_bp = (i & 1023) < 800 ? 3400 : 0;
        checksum += (unsigned)l1_step(&c, o);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double elapsed = (double)(b.tv_sec - a.tv_sec) +
                     (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    printf("L1 core microbenchmark: %" PRIu64 " decisions in %.6f s (%.1f ns/decision), checksum=%u\n",
           n, elapsed, elapsed * 1e9 / (double)n, checksum);
    puts("This measures only in-process L1 core cost, NOT PSI reads, power, or OS scheduling gains.");
    return 0;
}
