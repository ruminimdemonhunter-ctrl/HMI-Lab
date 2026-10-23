#define _POSIX_C_SOURCE 200809L
#include "l1_linux.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef enum { SOURCE_BASELINE, SOURCE_CANDIDATE } SourceMode;

static uint64_t process_ns(struct rusage usage) {
    return (uint64_t)usage.ru_utime.tv_sec * UINT64_C(1000000000) +
        (uint64_t)usage.ru_utime.tv_usec * 1000U +
        (uint64_t)usage.ru_stime.tv_sec * UINT64_C(1000000000) +
        (uint64_t)usage.ru_stime.tv_usec * 1000U;
}

static uint64_t monotonic_ns(struct timespec value) {
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

static bool positive_count(const char *text, uint32_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (unsigned)(*p - '0');
        if (value > 86400) return false;
    }
    if (value == 0) return false;
    *out = (uint32_t)value;
    return true;
}

static bool read_source(SourceMode mode, L1LinuxPsiReader *reader,
                        L1Observation *observation,
                        L1PressureCounters *counters) {
    return mode == SOURCE_BASELINE ?
        l1_read_linux_sample(observation, counters) :
        l1_linux_psi_reader_read(reader, observation, counters);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: l1_psi_bench baseline|candidate SAMPLES\n", stderr);
        return 2;
    }
    SourceMode mode;
    if (strcmp(argv[1], "baseline") == 0) mode = SOURCE_BASELINE;
    else if (strcmp(argv[1], "candidate") == 0) mode = SOURCE_CANDIDATE;
    else return 2;
    uint32_t samples;
    if (!positive_count(argv[2], &samples)) return 2;
    L1LinuxPsiReader reader = {-1, -1, -1};
    if (mode == SOURCE_CANDIDATE && !l1_linux_psi_reader_open(&reader)) return 1;
    L1Observation observation;
    L1PressureCounters counters;
    if (!read_source(mode, &reader, &observation, &counters)) {
        l1_linux_psi_reader_close(&reader);
        return 1;
    }
    struct rusage start, end;
    struct timespec wall_start, wall_end;
    if (getrusage(RUSAGE_SELF, &start) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_start) != 0) return 1;
    uint64_t checksum = UINT64_C(1469598103934665603);
    uint32_t valid = 0;
    for (uint32_t i = 0; i < samples; ++i) {
        if (read_source(mode, &reader, &observation, &counters)) valid++;
        checksum ^= observation.cpu_some_bp;
        checksum *= UINT64_C(1099511628211);
        checksum ^= observation.memory_full_bp;
        checksum *= UINT64_C(1099511628211);
        checksum ^= observation.io_full_bp;
        checksum *= UINT64_C(1099511628211);
        checksum ^= counters.cpu_some_total_us;
        checksum *= UINT64_C(1099511628211);
        checksum ^= counters.memory_full_total_us;
        checksum *= UINT64_C(1099511628211);
        checksum ^= counters.io_full_total_us;
        checksum *= UINT64_C(1099511628211);
    }
    if (getrusage(RUSAGE_SELF, &end) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_end) != 0) return 1;
    l1_linux_psi_reader_close(&reader);
    if (valid != samples) return 1;
    uint64_t cpu_ns = process_ns(end) - process_ns(start);
    uint64_t elapsed = monotonic_ns(wall_end) - monotonic_ns(wall_start);
    printf("PSI_SOURCE_BENCH,mode=%s,samples=%u,valid=%u,cpu_ns=%" PRIu64
           ",cpu_ns_per_sample=%.3f,wall_ns=%" PRIu64
           ",rss_kib=%ld,checksum=%016" PRIx64 "\n",
           mode == SOURCE_BASELINE ? "baseline" : "candidate", samples,
           valid, cpu_ns, (double)cpu_ns / samples, elapsed, end.ru_maxrss,
           checksum);
    return 0;
}
