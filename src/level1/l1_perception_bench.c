#define _POSIX_C_SOURCE 200809L
#include "l1_perception.h"

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

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s REPETITIONS CALLS_PER_REPETITION\n", argv[0]);
        return EXIT_FAILURE;
    }
    size_t repetitions = (size_t)parse_bounded(
        argv[1], 1, MAX_REPETITIONS, "repetitions");
    uint64_t calls = parse_bounded(argv[2], MIN_CALLS, MAX_CALLS, "calls");
    double values[MAX_REPETITIONS];
    uint64_t quantile_samples[MAX_QUANTILE_SAMPLES];
    size_t quantile_count = 0;
    volatile uint64_t sink = 0;

    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        uint64_t started = cpu_ns();
        for (uint64_t i = 0; i < calls; ++i) {
            uint16_t cpu = (uint16_t)((i * UINT64_C(1103)) % 10001U);
            uint16_t memory = (uint16_t)((i * UINT64_C(337)) % 10001U);
            uint16_t io = (uint16_t)((i * UINT64_C(719)) % 10001U);
            L1Observation observed = {cpu, memory, io, true};
            L1PerceptionResult classified = l1_perception_classify(
                observed, L1_PQ_OK);
            sink += (uint64_t)classified.state + classified.pressure_mask +
                    (uint64_t)classified.quality;
        }
        uint64_t elapsed = cpu_ns() - started;
        values[repetition] = (double)elapsed / (double)calls;
        printf("PERCEPTION_RUN repetition=%zu calls=%" PRIu64
               " cpu_ns_per_call=%.3f\n",
               repetition + 1U, calls, values[repetition]);

        size_t wanted = MAX_QUANTILE_SAMPLES / repetitions;
        if ((uint64_t)wanted > calls) {
            wanted = (size_t)calls;
        }
        for (size_t i = 0; i < wanted; ++i) {
            L1Observation observed = {
                (uint16_t)(((uint64_t)i * UINT64_C(1103)) % 10001U),
                (uint16_t)(((uint64_t)i * UINT64_C(337)) % 10001U),
                (uint16_t)(((uint64_t)i * UINT64_C(719)) % 10001U),
                true
            };
            uint64_t sample_started = cpu_ns();
            L1PerceptionResult classified = l1_perception_classify(
                observed, L1_PQ_OK);
            quantile_samples[quantile_count++] = cpu_ns() - sample_started;
            sink += (uint64_t)classified.state + classified.pressure_mask +
                    (uint64_t)classified.quality;
        }
    }

    qsort(values, repetitions, sizeof(values[0]), compare_double);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        return EXIT_FAILURE;
    }
    double median = values[repetitions / 2U];
    if ((repetitions & 1U) == 0U) {
        median = (values[repetitions / 2U - 1U] + median) / 2.0;
    }
    qsort(quantile_samples, quantile_count, sizeof(quantile_samples[0]),
          compare_u64);
    size_t p95_index = (quantile_count * 95U + 99U) / 100U - 1U;
    uint64_t p95 = quantile_samples[p95_index];
    printf("PERCEPTION_SUMMARY repetitions=%zu calls_per_repetition=%" PRIu64
           " min_cpu_ns_per_call=%.3f median_cpu_ns_per_call=%.3f"
           " max_cpu_ns_per_call=%.3f individual_samples=%zu"
           " individual_cpu_p95_ns=%" PRIu64 " rss_kib=%ld result_bytes=%zu"
           " persistent_state_bytes=0 sink=%" PRIu64 "\n",
           repetitions, calls, values[0], median, values[repetitions - 1U],
           quantile_count, p95, usage.ru_maxrss, sizeof(L1PerceptionResult),
           sink);
    if (median > 1000.0 || p95 > 5000U || usage.ru_maxrss > 8192) {
        fputs("FAIL: frozen perception resource budget exceeded\n", stderr);
        return EXIT_FAILURE;
    }
    puts("PASS: bounded stateless perception benchmark");
    return EXIT_SUCCESS;
}
