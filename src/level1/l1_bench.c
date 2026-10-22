#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef enum { ENGINE_L1, ENGINE_BASELINE } Engine;

static L1Action fixed_rule(L1Observation o) {
    if (!o.valid || o.cpu_some_bp > 10000 || o.memory_full_bp > 10000 ||
        o.io_full_bp > 10000) return L1_HOLD;
    if (o.memory_full_bp >= 100) return L1_PROTECT_MEMORY;
    if (o.io_full_bp >= 500) return L1_DEFER_IO;
    if (o.cpu_some_bp >= 3000) return L1_PREFER_INTERACTIVE;
    return L1_HOLD;
}

static double seconds_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) +
           (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static uint64_t parse_count(const char *text) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0) return 0;
    return (uint64_t)value;
}

static int run_core(Engine engine, uint64_t count) {
    L1Core core;
    l1_init(&core);
    L1Observation o = {0, 0, 0, true};
    struct timespec start, end;
    volatile unsigned checksum = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 0; i < count; ++i) {
        o.cpu_some_bp = (uint16_t)(((i & 1023U) < 800U) ? 3400 : 0);
        checksum += (unsigned)(engine == ENGINE_L1 ? l1_step(&core, o) :
                                                    fixed_rule(o));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = seconds_between(start, end);
    printf("scope,engine,samples,valid_observations,wall_seconds,cpu_seconds,ns_per_sample,max_rss_kib,checksum\n");
    printf("core,%s,%" PRIu64 ",NA,%.9f,NA,%.3f,NA,%u\n",
           engine == ENGINE_L1 ? "l1" : "fixed", count, elapsed,
           elapsed * 1e9 / (double)count, checksum);
    return 0;
}

static int run_live(Engine engine, uint64_t count) {
    L1Core core;
    l1_init(&core);
    struct timespec wall_start, wall_end, cpu_start, cpu_end;
    volatile unsigned checksum = 0;
    uint64_t valid_observations = 0;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    for (uint64_t i = 0; i < count; ++i) {
        L1Observation observation;
        if (l1_read_linux_observation(&observation)) ++valid_observations;
        checksum += (unsigned)(engine == ENGINE_L1 ? l1_step(&core, observation) :
                                                    fixed_rule(observation));
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 1;
    double wall = seconds_between(wall_start, wall_end);
    double cpu = seconds_between(cpu_start, cpu_end);
    printf("scope,engine,samples,valid_observations,wall_seconds,cpu_seconds,ns_per_sample,max_rss_kib,checksum\n");
    printf("observation-to-decision,%s,%" PRIu64 ",%" PRIu64 ",%.9f,%.9f,%.3f,%ld,%u\n",
           engine == ENGINE_L1 ? "l1" : "fixed", count,
           valid_observations, wall, cpu,
           cpu * 1e9 / (double)count, usage.ru_maxrss, checksum);
    return valid_observations == count ? 0 : 3;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "core") != 0 &&
                      strcmp(argv[1], "live") != 0) ||
        (strcmp(argv[2], "l1") != 0 && strcmp(argv[2], "fixed") != 0)) {
        fprintf(stderr, "Usage: %s {core|live} {l1|fixed} COUNT\n", argv[0]);
        return 2;
    }
    uint64_t count = parse_count(argv[3]);
    if (count == 0) return 2;
    Engine engine = strcmp(argv[2], "l1") == 0 ? ENGINE_L1 : ENGINE_BASELINE;
    return strcmp(argv[1], "core") == 0 ? run_core(engine, count) :
                                           run_live(engine, count);
}
