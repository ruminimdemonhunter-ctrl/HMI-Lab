#define _POSIX_C_SOURCE 200809L
#include "l1_operational_memory.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

enum {
    MIN_CALLS = 10000,
    MAX_CALLS = 100000000,
    MAX_REPETITIONS = 100,
    MAX_QUANTILE_SAMPLES = 50000
};

static uint64_t parse_bounded(const char *text, uint64_t minimum,
                              uint64_t maximum, const char *name) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || end == text || *end != '\0' ||
        text[0] == '-' || value < minimum || value > maximum) {
        fprintf(stderr, "invalid %s: expected %" PRIu64 "..%" PRIu64 "\n",
                name, minimum, maximum);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static uint64_t cpu_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static L1Observation benchmark_observation(uint64_t index) {
    L1Observation observation = {
        (uint16_t)((index * UINT64_C(1103)) % 10001U),
        (uint16_t)((index * UINT64_C(337)) % 10001U),
        (uint16_t)((index * UINT64_C(719)) % 10001U),
        true
    };
    return observation;
}

static double median(double *values, size_t count) {
    qsort(values, count, sizeof(values[0]), compare_double);
    double result = values[count / 2U];
    if ((count & 1U) == 0U) {
        result = (values[count / 2U - 1U] + result) / 2.0;
    }
    return result;
}

static uint64_t p95(uint64_t *values, size_t count) {
    qsort(values, count, sizeof(values[0]), compare_u64);
    size_t index = (count * 95U + 99U) / 100U - 1U;
    return values[index];
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s REPETITIONS CALLS_PER_REPETITION\n", argv[0]);
        return EXIT_FAILURE;
    }
    size_t repetitions = (size_t)parse_bounded(
        argv[1], 1, MAX_REPETITIONS, "repetitions");
    uint64_t calls = parse_bounded(argv[2], MIN_CALLS, MAX_CALLS, "calls");
    double perception_runs[MAX_REPETITIONS];
    double combined_runs[MAX_REPETITIONS];
    uint64_t perception_samples[MAX_QUANTILE_SAMPLES];
    uint64_t combined_samples[MAX_QUANTILE_SAMPLES];
    size_t sample_count = 0;
    volatile uint64_t sink = 0;

    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        uint64_t started = cpu_ns();
        for (uint64_t i = 0; i < calls; ++i) {
            L1PerceptionResult result = l1_perception_classify(
                benchmark_observation(i), L1_PQ_OK);
            sink += (uint64_t)result.state + result.pressure_mask;
        }
        perception_runs[repetition] =
            (double)(cpu_ns() - started) / (double)calls;

        L1OperationalMemory memory;
        l1_operational_memory_init(&memory);
        started = cpu_ns();
        for (uint64_t i = 0; i < calls; ++i) {
            L1Observation observation = benchmark_observation(i);
            L1PerceptionResult result = l1_perception_classify(
                observation, L1_PQ_OK);
            L1OperationalMemoryStatus status = l1_operational_memory_step(
                &memory, observation, L1_PQ_OK,
                i * (uint64_t)L1_OM_SAMPLE_INTERVAL_MS);
            sink += (uint64_t)result.state + result.pressure_mask + status;
        }
        combined_runs[repetition] =
            (double)(cpu_ns() - started) / (double)calls;
        printf("OPERATIONAL_MEMORY_RUN repetition=%zu calls=%" PRIu64
               " perception_cpu_ns_per_call=%.3f"
               " combined_cpu_ns_per_call=%.3f\n",
               repetition + 1U, calls, perception_runs[repetition],
               combined_runs[repetition]);

        size_t wanted = MAX_QUANTILE_SAMPLES / repetitions;
        if ((uint64_t)wanted > calls) {
            wanted = (size_t)calls;
        }
        l1_operational_memory_init(&memory);
        for (size_t i = 0; i < wanted; ++i) {
            L1Observation observation = benchmark_observation(i);
            uint64_t sample_started = cpu_ns();
            L1PerceptionResult perception = l1_perception_classify(
                observation, L1_PQ_OK);
            perception_samples[sample_count] = cpu_ns() - sample_started;

            sample_started = cpu_ns();
            L1PerceptionResult combined = l1_perception_classify(
                observation, L1_PQ_OK);
            L1OperationalMemoryStatus status = l1_operational_memory_step(
                &memory, observation, L1_PQ_OK,
                (uint64_t)i * L1_OM_SAMPLE_INTERVAL_MS);
            combined_samples[sample_count] = cpu_ns() - sample_started;
            sink += (uint64_t)perception.state + perception.pressure_mask +
                    (uint64_t)combined.state + combined.pressure_mask + status;
            ++sample_count;
        }
    }

    double perception_median = median(perception_runs, repetitions);
    double combined_median = median(combined_runs, repetitions);
    uint64_t perception_p95 = p95(perception_samples, sample_count);
    uint64_t combined_p95 = p95(combined_samples, sample_count);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        return EXIT_FAILURE;
    }
    printf("OPERATIONAL_MEMORY_SUMMARY repetitions=%zu calls_per_repetition=%" PRIu64
           " perception_cpu_ns=%.3f/%.3f/%.3f"
           " combined_cpu_ns=%.3f/%.3f/%.3f individual_samples=%zu"
           " perception_individual_p95_ns=%" PRIu64
           " combined_individual_p95_ns=%" PRIu64
           " rss_kib=%ld memory_bytes=%zu summary_bytes=%zu"
           " checkpoint_bytes=%d sink=%" PRIu64 "\n",
           repetitions, calls, perception_runs[0], perception_median,
           perception_runs[repetitions - 1U], combined_runs[0],
           combined_median, combined_runs[repetitions - 1U], sample_count,
           perception_p95, combined_p95, usage.ru_maxrss,
           sizeof(L1OperationalMemory), sizeof(L1OperationalMemorySummary),
           L1_OM_CHECKPOINT_BYTES, sink);
    if (combined_median > 1000.0 || combined_p95 > 5000U ||
        usage.ru_maxrss > 8192 || sizeof(L1OperationalMemory) > 512U) {
        fputs("FAIL: frozen R2.2 resource budget exceeded\n", stderr);
        return EXIT_FAILURE;
    }
    puts("PASS: bounded operational-memory perception benchmark");
    return EXIT_SUCCESS;
}
