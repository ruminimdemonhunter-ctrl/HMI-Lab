#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { TRACE_SAMPLES = 1240, EVENT_START = 1180, EVENT_END = 1210 };
static const uint64_t max_repetitions = UINT64_C(100000);

typedef struct {
    uint64_t false_positive_samples;
    uint64_t missed_response_samples;
    uint64_t oscillations;
    uint64_t detection_delay_sum;
    uint64_t detection_events;
    uint64_t max_detection_delay;
    uint8_t final_debounce_extra;
} ReplayMetrics;

static L1Observation replay_observation(size_t sample) {
    bool high;
    if (sample <= 600U) {
        high = (sample / 4U) % 2U == 0;
    } else if (sample < 1160U) {
        high = ((sample - 601U) % 8U) < 3U;
    } else {
        high = sample >= EVENT_START && sample < EVENT_END;
    }
    return (L1Observation){high ? 5000U : 0U, 0, 0, true};
}

static L1Action expected_action(size_t sample) {
    return sample >= EVENT_START && sample < EVENT_END ?
           L1_PREFER_INTERACTIVE : L1_HOLD;
}

static unsigned run_replay(bool adaptive, ReplayMetrics *metrics) {
    L1Core core;
    l1_init(&core);
    memset(metrics, 0, sizeof(*metrics));
    L1Action previous_actual = L1_HOLD;
    L1Action previous_expected = L1_HOLD;
    bool detection_pending = false;
    size_t transition_sample = 0;
    unsigned checksum = 0;
    for (size_t i = 0; i < TRACE_SAMPLES; ++i) {
        L1Action expected = expected_action(i);
        if (expected != previous_expected) {
            detection_pending = true;
            transition_sample = i;
            previous_expected = expected;
        }
        /* A constant monotonic timestamp is the deterministic fixed-memory
           control: decisions run normally but the adaptation time gate cannot pass. */
        uint64_t timestamp_ms = adaptive ? (uint64_t)i * 1000U : 0;
        L1Action actual = l1_step_at(&core, replay_observation(i), timestamp_ms);
        checksum += (unsigned)actual;
        if (i != 0 && actual != previous_actual) metrics->oscillations++;
        previous_actual = actual;
        if (expected == L1_HOLD && actual != L1_HOLD) {
            metrics->false_positive_samples++;
        } else if (expected != L1_HOLD && actual != expected) {
            metrics->missed_response_samples++;
        }
        if (detection_pending && actual == expected) {
            uint64_t delay = (uint64_t)(i - transition_sample);
            metrics->detection_delay_sum += delay;
            metrics->detection_events++;
            if (delay > metrics->max_detection_delay) {
                metrics->max_detection_delay = delay;
            }
            detection_pending = false;
        }
    }
    metrics->final_debounce_extra = core.debounce_extra;
    return checksum;
}

static bool parse_repetitions(const char *text, uint64_t *result) {
    if (text == NULL || result == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0 ||
        value > max_repetitions || (uint64_t)value != value) return false;
    *result = (uint64_t)value;
    return true;
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static double benchmark_replay(bool adaptive, uint64_t repetitions,
                               ReplayMetrics *first, unsigned *checksum) {
    struct timespec start, end;
    ReplayMetrics metrics;
    volatile unsigned accumulated = 0;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    for (uint64_t i = 0; i < repetitions; ++i) {
        accumulated += run_replay(adaptive, &metrics);
        if (i == 0) *first = metrics;
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    *checksum = accumulated;
    return elapsed_seconds(start, end) * 1e9 /
           ((double)repetitions * TRACE_SAMPLES);
}

static void print_result(const char *engine, const ReplayMetrics *metrics,
                         double cpu_ns, long rss, unsigned checksum) {
    double mean_delay = metrics->detection_events == 0 ? 0.0 :
        (double)metrics->detection_delay_sum / (double)metrics->detection_events;
    printf("%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%" PRIu64
           ",%u,%zu,%.3f,%ld,%u\n",
           engine, metrics->false_positive_samples,
           metrics->missed_response_samples, metrics->oscillations,
           mean_delay, metrics->max_detection_delay,
           metrics->final_debounce_extra, sizeof(L1Core), cpu_ns, rss, checksum);
}

int main(int argc, char **argv) {
    uint64_t repetitions = 10000;
    if (argc > 2 || (argc == 2 && !parse_repetitions(argv[1], &repetitions))) {
        fprintf(stderr, "Usage: %s [REPETITIONS 1..100000]\n", argv[0]);
        return 2;
    }
    ReplayMetrics fixed, adaptive;
    unsigned fixed_checksum, adaptive_checksum;
    double fixed_cpu = benchmark_replay(false, repetitions, &fixed,
                                        &fixed_checksum);
    double adaptive_cpu = benchmark_replay(true, repetitions, &adaptive,
                                           &adaptive_checksum);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 1;
    puts("engine,false_positive_samples,missed_response_samples,oscillations,mean_detection_delay_samples,max_detection_delay_samples,final_debounce_extra,core_bytes,cpu_ns_per_decision,max_rss_kib,checksum");
    print_result("fixed-memory", &fixed, fixed_cpu, usage.ru_maxrss,
                 fixed_checksum);
    print_result("bounded-adaptive", &adaptive, adaptive_cpu, usage.ru_maxrss,
                 adaptive_checksum);
    if (adaptive.final_debounce_extra != 1 ||
        adaptive.false_positive_samples >= fixed.false_positive_samples ||
        adaptive.oscillations >= fixed.oscillations ||
        adaptive.detection_events != 2 || fixed.detection_events != 2) {
        fputs("FAIL: replay did not meet its predeclared stability criteria\n", stderr);
        return 1;
    }
    puts("PASS: bounded adaptation reduced false responses and oscillation on the declared trace");
    return 0;
}
