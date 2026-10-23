#define _POSIX_C_SOURCE 200809L
#include "l1.h"
#include "l1_shadow.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { MAX_LIVE_SAMPLES = 1000000 };
static const uint64_t max_core_samples = UINT64_C(1000000000);

static double seconds_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) +
           (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static bool parse_count(const char *text, uint64_t maximum, uint64_t *result) {
    if (text == NULL || result == NULL || text[0] < '0' || text[0] > '9')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0 ||
        value > maximum || (uint64_t)value != value) return false;
    *result = (uint64_t)value;
    return true;
}

static int run_core(bool with_shadow, uint64_t count) {
    L1Core core;
    L1Shadow shadow;
    L1Observation observation = {0, 0, 0, true};
    L1PressureCounters counters = {0, 0, 0, true};
    struct timespec start, end;
    volatile unsigned checksum = 0;
    l1_init(&core);
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 0; i < count; ++i) {
        observation.cpu_some_bp = (i % 180U < 60U) ? 1400 : 100;
        observation.memory_full_bp = (i % 700U < 80U) ? 120 : 2;
        observation.io_full_bp = (i % 260U < 70U) ? 450 : 10;
        counters.cpu_some_total_us += observation.cpu_some_bp * 100U;
        counters.memory_full_total_us += observation.memory_full_bp * 100U;
        counters.io_full_total_us += observation.io_full_bp * 100U;
        uint64_t now = i * 1000U;
        checksum += (unsigned)l1_step_at(&core, observation, now);
        if (with_shadow) {
            L1ShadowStep step = l1_shadow_step_at(
                &shadow, observation, counters, now);
            checksum += step.feedback_ready ? step.feedback.positive_mask : 0U;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = seconds_between(start, end);
    printf("scope,engine,samples,valid_observations,wall_seconds,cpu_seconds,ns_per_sample,max_rss_kib,checksum\n");
    printf("shadow-core,%s,%" PRIu64 ",NA,%.9f,NA,%.3f,NA,%u\n",
           with_shadow ? "l1-shadow" : "l1", count, elapsed,
           elapsed * 1e9 / (double)count, checksum);
    return 0;
}

static int run_live(bool with_shadow, uint64_t count) {
    L1Core core;
    L1Shadow shadow;
    struct timespec wall_start, wall_end, cpu_start, cpu_end;
    volatile unsigned checksum = 0;
    uint64_t valid = 0;
    l1_init(&core);
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    for (uint64_t i = 0; i < count; ++i) {
        L1Observation observation;
        L1PressureCounters counters;
        if (l1_read_linux_sample(&observation, &counters)) valid++;
        struct timespec sampled;
        clock_gettime(CLOCK_MONOTONIC, &sampled);
        uint64_t now = (uint64_t)sampled.tv_sec * UINT64_C(1000) +
                       (uint64_t)sampled.tv_nsec / UINT64_C(1000000);
        checksum += (unsigned)l1_step_at(&core, observation, now);
        if (with_shadow) {
            L1ShadowStep step = l1_shadow_step_at(
                &shadow, observation, counters, now);
            checksum += step.feedback_ready ? step.feedback.positive_mask : 0U;
        }
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 1;
    double wall = seconds_between(wall_start, wall_end);
    double cpu = seconds_between(cpu_start, cpu_end);
    printf("scope,engine,samples,valid_observations,wall_seconds,cpu_seconds,ns_per_sample,max_rss_kib,checksum\n");
    printf("observation-shadow,%s,%" PRIu64 ",%" PRIu64 ",%.9f,%.9f,%.3f,%ld,%u\n",
           with_shadow ? "l1-shadow" : "l1", count, valid, wall, cpu,
           cpu * 1e9 / (double)count, usage.ru_maxrss, checksum);
    return valid == count ? 0 : 3;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "core") && strcmp(argv[1], "live")) ||
        (strcmp(argv[2], "l1") && strcmp(argv[2], "l1-shadow"))) {
        fprintf(stderr, "Usage: %s {core|live} {l1|l1-shadow} COUNT\n", argv[0]);
        return 2;
    }
    uint64_t maximum = !strcmp(argv[1], "core") ? max_core_samples :
                                                   MAX_LIVE_SAMPLES;
    uint64_t count;
    if (!parse_count(argv[3], maximum, &count)) return 2;
    bool with_shadow = !strcmp(argv[2], "l1-shadow");
    return !strcmp(argv[1], "core") ? run_core(with_shadow, count) :
                                       run_live(with_shadow, count);
}
