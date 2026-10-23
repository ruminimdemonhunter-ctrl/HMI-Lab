#include "l1_shadow.h"

#include <limits.h>
#include <string.h>

static const uint16_t outcome_thresholds[L1_FORECAST_CLASS_COUNT] =
    {800, 40, 150};
static const uint16_t offset_steps[L1_FORECAST_CLASS_COUNT] =
    {200, 10, 40};

static void increment_u16(uint16_t *value) {
    if (*value < UINT16_MAX) (*value)++;
}

static void increment_u32(uint32_t *value) {
    if (*value < UINT32_MAX) (*value)++;
}

static uint64_t counter_for(const L1PressureCounters *counters, unsigned c) {
    if (c == L1_FORECAST_CPU) return counters->cpu_some_total_us;
    if (c == L1_FORECAST_MEMORY) return counters->memory_full_total_us;
    return counters->io_full_total_us;
}

static uint16_t observed_for(L1Observation observation, unsigned c) {
    if (c == L1_FORECAST_CPU) return observation.cpu_some_bp;
    if (c == L1_FORECAST_MEMORY) return observation.memory_full_bp;
    return observation.io_full_bp;
}

static uint16_t base_for(L1ForecastThresholds thresholds, unsigned c) {
    if (c == L1_FORECAST_CPU) return thresholds.cpu_bp;
    if (c == L1_FORECAST_MEMORY) return thresholds.memory_bp;
    return thresholds.io_bp;
}

static uint16_t effective_threshold(const L1Shadow *shadow, unsigned c) {
    int32_t value = (int32_t)base_for(shadow->base_thresholds, c) +
        (int32_t)shadow->offset[c] * (int32_t)offset_steps[c];
    if (value < 1) value = 1;
    if (value > 10000) value = 10000;
    return (uint16_t)value;
}

L1ForecastThresholds l1_default_forecast_thresholds(void) {
    return (L1ForecastThresholds){800, 40, 150};
}

bool l1_forecast_thresholds_valid(L1ForecastThresholds thresholds) {
    return thresholds.cpu_bp >= 1 && thresholds.cpu_bp <= 10000 &&
        thresholds.memory_bp >= 1 && thresholds.memory_bp <= 10000 &&
        thresholds.io_bp >= 1 && thresholds.io_bp <= 10000;
}

void l1_shadow_init(L1Shadow *shadow, L1ForecastThresholds thresholds) {
    if (shadow == NULL) return;
    memset(shadow, 0, sizeof(*shadow));
    shadow->base_thresholds = l1_forecast_thresholds_valid(thresholds) ?
        thresholds : l1_default_forecast_thresholds();
}

void l1_shadow_reset(L1Shadow *shadow) {
    if (shadow == NULL) return;
    L1ForecastThresholds thresholds = shadow->base_thresholds;
    uint32_t resets = shadow->reset_count;
    uint32_t predictions = shadow->predictions_issued;
    uint32_t feedback = shadow->feedback_windows;
    uint32_t invalid = shadow->discarded_invalid;
    uint32_t gap_or_late = shadow->discarded_gap_or_late;
    uint32_t discarded_reset = shadow->discarded_reset;
    uint32_t updates[L1_FORECAST_CLASS_COUNT];
    memcpy(updates, shadow->updates, sizeof(updates));
    l1_shadow_init(shadow, thresholds);
    shadow->reset_count = resets;
    shadow->predictions_issued = predictions;
    shadow->feedback_windows = feedback;
    shadow->discarded_invalid = invalid;
    shadow->discarded_gap_or_late = gap_or_late;
    shadow->discarded_reset = discarded_reset;
    memcpy(shadow->updates, updates, sizeof(updates));
}

bool l1_fixed_prediction(L1Observation observation,
                         L1ForecastThresholds thresholds,
                         L1Prediction *prediction) {
    if (prediction == NULL || !observation.valid ||
        observation.cpu_some_bp > 10000 ||
        observation.memory_full_bp > 10000 ||
        observation.io_full_bp > 10000 ||
        !l1_forecast_thresholds_valid(thresholds)) return false;
    memset(prediction, 0, sizeof(*prediction));
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        uint16_t threshold = base_for(thresholds, c);
        prediction->effective_threshold_bp[c] = threshold;
        if (observed_for(observation, c) >= threshold)
            prediction->positive_mask |= (uint8_t)(1U << c);
    }
    return true;
}

static bool counters_decreased(const L1PressureCounters *now,
                               const L1PressureCounters *before) {
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c)
        if (counter_for(now, c) < counter_for(before, c)) return true;
    return false;
}

static bool interval_possible(const L1PressureCounters *now,
                              const L1PressureCounters *before,
                              uint64_t elapsed_ms) {
    if (elapsed_ms > UINT64_MAX / 1000U) return false;
    uint64_t elapsed_us = elapsed_ms * 1000U;
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        uint64_t current = counter_for(now, c);
        uint64_t prior = counter_for(before, c);
        if (current < prior || current - prior > elapsed_us) return false;
    }
    return true;
}

static void hard_reset(L1Shadow *shadow) {
    if (shadow->pending) increment_u32(&shadow->discarded_reset);
    increment_u32(&shadow->reset_count);
    l1_shadow_reset(shadow);
}

static L1Prediction make_prediction(const L1Shadow *shadow,
                                    L1Observation observation) {
    L1Prediction prediction;
    memset(&prediction, 0, sizeof(prediction));
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        uint16_t threshold = effective_threshold(shadow, c);
        prediction.effective_threshold_bp[c] = threshold;
        if (observed_for(observation, c) >= threshold)
            prediction.positive_mask |= (uint8_t)(1U << c);
    }
    return prediction;
}

static bool feedback_from_window(const L1Shadow *shadow,
                                 const L1PressureCounters *end,
                                 uint64_t elapsed_ms,
                                 L1Feedback *feedback) {
    if (!interval_possible(end, &shadow->pending_counters, elapsed_ms))
        return false;
    uint64_t elapsed_us = elapsed_ms * 1000U;
    memset(feedback, 0, sizeof(*feedback));
    feedback->available = true;
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        uint64_t delta = counter_for(end, c) -
            counter_for(&shadow->pending_counters, c);
        uint16_t bp = (uint16_t)((delta * 10000U) / elapsed_us);
        feedback->pressure_bp[c] = bp;
        if (bp >= outcome_thresholds[c])
            feedback->positive_mask |= (uint8_t)(1U << c);
    }
    return true;
}

static void maybe_adapt(L1Shadow *shadow, unsigned c, uint64_t now_ms) {
    if (shadow->valid_samples[c] < L1_SELF_ADAPT_MIN_SAMPLES ||
        !shadow->gate_started[c] ||
        now_ms - shadow->gate_started_ms[c] <
            L1_SELF_ADAPT_MIN_INTERVAL_MS ||
        shadow->evaluated_windows[c] < L1_FORECAST_MIN_WINDOWS) return;
    uint16_t false_alarms = shadow->false_alarms[c];
    uint16_t misses = shadow->misses[c];
    if ((uint32_t)misses >= (uint32_t)false_alarms + 2U &&
        shadow->offset[c] > -L1_FORECAST_MAX_OFFSET) {
        shadow->offset[c]--;
        increment_u32(&shadow->updates[c]);
    } else if ((uint32_t)false_alarms >= (uint32_t)misses + 2U &&
               shadow->offset[c] < L1_FORECAST_MAX_OFFSET) {
        shadow->offset[c]++;
        increment_u32(&shadow->updates[c]);
    }
    shadow->false_alarms[c] = 0;
    shadow->misses[c] = 0;
    shadow->evaluated_windows[c] = 0;
    shadow->valid_samples[c] = 0;
    shadow->gate_started_ms[c] = now_ms;
}

static void apply_feedback(L1Shadow *shadow, const L1Feedback *feedback,
                           uint64_t now_ms) {
    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        bool predicted = (shadow->pending_prediction.positive_mask &
                          (uint8_t)(1U << c)) != 0;
        bool outcome = (feedback->positive_mask & (uint8_t)(1U << c)) != 0;
        if (predicted && !outcome) increment_u16(&shadow->false_alarms[c]);
        if (!predicted && outcome) increment_u16(&shadow->misses[c]);
        increment_u16(&shadow->evaluated_windows[c]);
        maybe_adapt(shadow, c, now_ms);
    }
    increment_u32(&shadow->feedback_windows);
}

static void issue_prediction(L1Shadow *shadow, L1Observation observation,
                             L1PressureCounters counters, uint64_t now_ms,
                             L1ShadowStep *step) {
    shadow->pending_prediction = make_prediction(shadow, observation);
    shadow->pending_counters = counters;
    shadow->pending_started_ms = now_ms;
    shadow->pending = true;
    step->prediction_issued = true;
    step->prediction = shadow->pending_prediction;
    increment_u32(&shadow->predictions_issued);
}

L1ShadowStep l1_shadow_step_at(L1Shadow *shadow, L1Observation observation,
                               L1PressureCounters counters,
                               uint64_t monotonic_ms) {
    L1ShadowStep step;
    memset(&step, 0, sizeof(step));
    if (shadow == NULL) return step;

    bool sample_valid = observation.valid && counters.valid &&
        observation.cpu_some_bp <= 10000 &&
        observation.memory_full_bp <= 10000 &&
        observation.io_full_bp <= 10000;
    if (!sample_valid) {
        if (shadow->pending) {
            shadow->pending = false;
            increment_u32(&shadow->discarded_invalid);
        }
        return step;
    }

    if (shadow->monotonic_initialized &&
        monotonic_ms < shadow->last_monotonic_ms) {
        hard_reset(shadow);
        shadow->monotonic_initialized = true;
        shadow->last_monotonic_ms = monotonic_ms;
        shadow->last_counters = counters;
        return step;
    }

    if (shadow->monotonic_initialized) {
        uint64_t elapsed = monotonic_ms - shadow->last_monotonic_ms;
        if (counters_decreased(&counters, &shadow->last_counters) ||
            !interval_possible(&counters, &shadow->last_counters, elapsed)) {
            hard_reset(shadow);
            shadow->monotonic_initialized = true;
            shadow->last_monotonic_ms = monotonic_ms;
            shadow->last_counters = counters;
            return step;
        }
        if (elapsed > L1_FORECAST_MAX_GAP_MS && shadow->pending) {
            shadow->pending = false;
            increment_u32(&shadow->discarded_gap_or_late);
        }
    }
    shadow->monotonic_initialized = true;
    shadow->last_monotonic_ms = monotonic_ms;
    shadow->last_counters = counters;

    for (unsigned c = 0; c < L1_FORECAST_CLASS_COUNT; ++c) {
        if (!shadow->gate_started[c]) {
            shadow->gate_started[c] = true;
            shadow->gate_started_ms[c] = monotonic_ms;
        }
        increment_u32(&shadow->valid_samples[c]);
    }

    if (shadow->pending) {
        uint64_t elapsed = monotonic_ms - shadow->pending_started_ms;
        if (elapsed > L1_FORECAST_LATEST_MS) {
            shadow->pending = false;
            increment_u32(&shadow->discarded_gap_or_late);
        } else if (elapsed >= L1_FORECAST_HORIZON_MS) {
            L1Feedback feedback;
            if (!feedback_from_window(shadow, &counters, elapsed, &feedback)) {
                hard_reset(shadow);
                shadow->monotonic_initialized = true;
                shadow->last_monotonic_ms = monotonic_ms;
                shadow->last_counters = counters;
                return step;
            }
            step.feedback_ready = true;
            step.feedback = feedback;
            apply_feedback(shadow, &feedback, monotonic_ms);
            shadow->pending = false;
        }
    }
    if (!shadow->pending)
        issue_prediction(shadow, observation, counters, monotonic_ms, &step);
    return step;
}
