#include "l1_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static L1Observation observation(uint16_t cpu, uint16_t memory, uint16_t io) {
    return (L1Observation){cpu, memory, io, true};
}

static L1PressureCounters counters(uint64_t cpu, uint64_t memory,
                                   uint64_t io) {
    return (L1PressureCounters){cpu, memory, io, true};
}

static void test_causal_ordering_and_matching(void) {
    L1Shadow shadow;
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());
    L1ShadowStep step = l1_shadow_step_at(
        &shadow, observation(900, 50, 200), counters(0, 0, 0), 0);
    CHECK(step.prediction_issued && !step.feedback_ready);
    CHECK(step.prediction.positive_mask == 7U);

    for (uint64_t i = 1; i < 60; ++i) {
        step = l1_shadow_step_at(&shadow, observation(0, 0, 0),
            counters(90000U * i, 2000U * i, 20000U * i), 1000U * i);
        CHECK(!step.prediction_issued && !step.feedback_ready);
        CHECK(shadow.evaluated_windows[0] == 0);
    }
    step = l1_shadow_step_at(&shadow, observation(0, 0, 0),
                             counters(5400000, 120000, 1200000), 60000);
    CHECK(step.feedback_ready && step.prediction_issued);
    CHECK(step.feedback.available);
    CHECK(step.feedback.positive_mask ==
          ((1U << L1_FORECAST_CPU) | (1U << L1_FORECAST_IO)));
    CHECK(step.feedback.pressure_bp[L1_FORECAST_CPU] == 900);
    CHECK(step.feedback.pressure_bp[L1_FORECAST_MEMORY] == 20);
    CHECK(step.feedback.pressure_bp[L1_FORECAST_IO] == 200);
    CHECK(shadow.false_alarms[L1_FORECAST_MEMORY] == 1);
    CHECK(shadow.feedback_windows == 1);
}

static void test_missing_gap_reset_and_regression(void) {
    L1Shadow shadow;
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(100, 100, 100), 1000);
    L1Observation invalid = observation(0, 0, 0);
    invalid.valid = false;
    L1PressureCounters valid_counters = counters(100, 100, 100);
    (void)l1_shadow_step_at(&shadow, invalid, valid_counters, 2000);
    CHECK(!shadow.pending && shadow.discarded_invalid == 1);
    CHECK(shadow.feedback_windows == 0);

    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(200, 200, 200), 3000);
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(300, 300, 300), 9001);
    CHECK(shadow.discarded_gap_or_late == 1);
    CHECK(shadow.pending); /* Current valid sample starts a fresh window. */

    shadow.offset[L1_FORECAST_CPU] = 2;
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(250, 300, 300), 10001);
    CHECK(shadow.reset_count == 1);
    CHECK(shadow.discarded_reset == 1);
    CHECK(shadow.offset[L1_FORECAST_CPU] == 0 && !shadow.pending);

    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(400, 400, 400), 11000);
    shadow.offset[L1_FORECAST_IO] = -2;
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(400, 400, 400), 10500);
    CHECK(shadow.reset_count == 2);
    CHECK(shadow.discarded_reset == 2);
    CHECK(shadow.offset[L1_FORECAST_IO] == 0 && !shadow.pending);

    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(500, 500, 500), 12000);
    shadow.offset[L1_FORECAST_MEMORY] = 1;
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(2000500, 500, 500), 13000);
    CHECK(shadow.reset_count == 3);
    CHECK(shadow.discarded_reset == 3);
    CHECK(shadow.offset[L1_FORECAST_MEMORY] == 0 && !shadow.pending);
}

static void prepare_gate(L1Shadow *shadow, int8_t offset,
                         uint16_t false_alarms, uint16_t misses,
                         uint8_t pending_mask, uint8_t outcome_mask) {
    l1_shadow_init(shadow, l1_default_forecast_thresholds());
    shadow->offset[L1_FORECAST_CPU] = offset;
    shadow->false_alarms[L1_FORECAST_CPU] = false_alarms;
    shadow->misses[L1_FORECAST_CPU] = misses;
    shadow->evaluated_windows[L1_FORECAST_CPU] = 7;
    shadow->valid_samples[L1_FORECAST_CPU] = 599;
    shadow->gate_started[L1_FORECAST_CPU] = true;
    shadow->gate_started_ms[L1_FORECAST_CPU] = 0;
    shadow->pending = true;
    shadow->pending_started_ms = 540000;
    shadow->pending_counters = counters(0, 0, 0);
    shadow->pending_prediction.positive_mask = pending_mask;
    shadow->monotonic_initialized = true;
    shadow->last_monotonic_ms = 599000;
    uint64_t end_cpu = outcome_mask ? 5400000U : 0U;
    shadow->last_counters = counters(end_cpu, 0, 0);
}

static void test_gate_and_saturation(void) {
    L1Shadow shadow;
    prepare_gate(&shadow, 0, 0, 7, 0, 1);
    L1ShadowStep step = l1_shadow_step_at(
        &shadow, observation(0, 0, 0), counters(5400000, 0, 0), 600000);
    CHECK(step.feedback_ready);
    CHECK(shadow.offset[L1_FORECAST_CPU] == -1);
    CHECK(shadow.updates[L1_FORECAST_CPU] == 1);

    prepare_gate(&shadow, -2, 0, 7, 0, 1);
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(5400000, 0, 0), 600000);
    CHECK(shadow.offset[L1_FORECAST_CPU] == -2);
    CHECK(shadow.updates[L1_FORECAST_CPU] == 0);

    prepare_gate(&shadow, 0, 7, 0, 1, 0);
    (void)l1_shadow_step_at(&shadow, observation(0, 0, 0),
                            counters(0, 0, 0), 600000);
    CHECK(shadow.offset[L1_FORECAST_CPU] == 1);
}

static void test_published_actions_unchanged(void) {
    L1Core ordinary, shadowed;
    L1Shadow shadow;
    l1_init(&ordinary);
    l1_init(&shadowed);
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());
    uint64_t total[3] = {0, 0, 0};
    for (uint64_t i = 0; i < 1800; ++i) {
        L1Observation o = observation(
            i % 90U < 30U ? 5000 : 0,
            i % 500U >= 450U ? 200 : 0,
            i % 140U < 50U ? 900 : 0);
        total[0] += i % 90U < 30U ? 180000U : 10000U;
        total[1] += i % 500U >= 450U ? 12000U : 200U;
        total[2] += i % 140U < 50U ? 45000U : 1000U;
        uint64_t now = i * 1000U;
        CHECK(l1_step_at(&ordinary, o, now) ==
              l1_step_at(&shadowed, o, now));
        (void)l1_shadow_step_at(&shadow, o,
            counters(total[0], total[1], total[2]), now);
        CHECK(memcmp(&ordinary, &shadowed, sizeof(ordinary)) == 0);
    }
}

int main(void) {
    CHECK(sizeof(L1Shadow) <= 512);
    test_causal_ordering_and_matching();
    test_missing_gap_reset_and_regression();
    test_gate_and_saturation();
    test_published_actions_unchanged();
    puts("PASS: causal delayed feedback, reset/saturation, and unchanged actions");
    return 0;
}
