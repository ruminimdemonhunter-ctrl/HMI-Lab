#include "l1.h"

#include <stdio.h>

typedef struct {
    L1Observation observation;
    L1Action expected;
} TraceStep;

#define OBS(cpu, mem, io) { (cpu), (mem), (io), true }

static int run_trace(const char *name, const TraceStep *trace, size_t count) {
    L1Core core;
    l1_init(&core);
    for (size_t i = 0; i < count; ++i) {
        L1Action actual = l1_step(&core, trace[i].observation);
        if (actual != trace[i].expected) {
            fprintf(stderr, "FAIL: %s step %zu expected %s, got %s\n",
                    name, i + 1, l1_action_name(trace[i].expected),
                    l1_action_name(actual));
            return 1;
        }
    }
    printf("PASS: %s (%zu known decisions)\n", name, count);
    return 0;
}

int main(void) {
    static const TraceStep cpu_trace[] = {
        {OBS(0, 0, 0), L1_HOLD},
        {OBS(3000, 0, 0), L1_HOLD},
        {OBS(3200, 0, 0), L1_HOLD},
        {OBS(3100, 0, 0), L1_PREFER_INTERACTIVE},
        {OBS(2750, 0, 0), L1_PREFER_INTERACTIVE},
        {OBS(2600, 0, 0), L1_PREFER_INTERACTIVE},
        {OBS(0, 0, 0), L1_PREFER_INTERACTIVE},
        {OBS(0, 0, 0), L1_HOLD}
    };
    static const TraceStep priority_trace[] = {
        {OBS(9000, 0, 0), L1_HOLD},
        {OBS(9000, 0, 0), L1_HOLD},
        {OBS(9000, 0, 0), L1_PREFER_INTERACTIVE},
        {OBS(9000, 0, 900), L1_PREFER_INTERACTIVE},
        {OBS(9000, 0, 900), L1_PREFER_INTERACTIVE},
        {OBS(9000, 0, 900), L1_DEFER_IO},
        {OBS(9000, 200, 900), L1_DEFER_IO},
        {OBS(9000, 200, 900), L1_DEFER_IO},
        {OBS(9000, 200, 900), L1_PROTECT_MEMORY}
    };
    static const TraceStep invalid_trace[] = {
        {OBS(5000, 0, 0), L1_HOLD},
        {OBS(5000, 0, 0), L1_HOLD},
        {{5000, 0, 0, false}, L1_HOLD},
        {OBS(5000, 0, 0), L1_HOLD},
        {OBS(5000, 0, 0), L1_HOLD},
        {OBS(5000, 0, 0), L1_PREFER_INTERACTIVE}
    };
    int failed = 0;
    failed |= run_trace("CPU threshold and hysteresis trace", cpu_trace,
                        sizeof(cpu_trace) / sizeof(cpu_trace[0]));
    failed |= run_trace("memory/I/O/CPU priority trace", priority_trace,
                        sizeof(priority_trace) / sizeof(priority_trace[0]));
    failed |= run_trace("invalid telemetry reset trace", invalid_trace,
                        sizeof(invalid_trace) / sizeof(invalid_trace[0]));
    return failed;
}
