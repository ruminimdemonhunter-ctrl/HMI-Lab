#define _POSIX_C_SOURCE 200809L
#include "../l1.h"
#include "../l1_shadow.h"
#include "../l1_trace.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

enum { CLASS_COUNT = 3, FAMILY_COUNT = 10, MAX_EVENTS = 32, MAX_SEEDS = 5 };
enum { MAX_SCENARIOS = MAX_SEEDS * FAMILY_COUNT, ELAPSED_BUCKETS = 6 };
static const uint64_t development_seeds[] = {701, 709};
static const uint64_t verification_seeds[] = {7001, 7013, 7027};
static const uint64_t heldout_seeds[] = {17011, 18013, 19031, 20047, 21059};
static const size_t family_lengths[] =
    {1200, 2400, 3000, 3000, 3000, 3600, 4800, 5400, 3000, 3000};
static const char *family_names[] = {
    "idle", "short-bursts", "sustained-cpu", "sustained-memory",
    "sustained-io", "transitions-overlap", "long-regime-shifts",
    "near-threshold-learning", "invalid-and-gaps",
    "regression-reset-corruption"
};
static const char *class_names[] = {"cpu", "memory", "io"};
static const L1ForecastThresholds configurations[] =
    {{800, 40, 150}, {2000, 75, 600}};
static const char *configuration_names[] = {"live-default", "v06-trained"};

/* Filled and committed after identity-only generation, before held-out scoring. */
static const uint64_t frozen_development_stream = UINT64_C(0xdb58eecd0c03ae79);
static const uint64_t frozen_development_schedule = UINT64_C(0x7bac0d8fe83eced2);
static const uint64_t frozen_verification_stream = UINT64_C(0x89a76049e04fc3e7);
static const uint64_t frozen_verification_schedule = UINT64_C(0x07867dce0615e19c);
static const uint64_t frozen_heldout_stream = UINT64_C(0xaed51578256958b0);
static const uint64_t frozen_heldout_schedule = UINT64_C(0x85c31afc0b01be04);

typedef struct { uint64_t state, increment; } Pcg32;
typedef struct { uint8_t class_id; uint32_t start, duration; } Event;
typedef struct {
    Event events[MAX_EVENTS];
    unsigned event_count;
    uint32_t regime[3];
    uint32_t faults[6];
    unsigned fault_count;
    uint64_t fingerprint;
} Schedule;
typedef struct {
    uint64_t issued, evaluated, invalid, gap_late, regression, reset;
    uint64_t impossible, unfinished, observations;
    uint64_t static_tp[3], static_tn[3], static_fp[3], static_fn[3];
    uint64_t learner_tp[3], learner_tn[3], learner_fp[3], learner_fn[3];
    uint64_t updates[3], offset_windows[3], boundary[3], issued_disagreements[3];
    uint64_t disagreements[3];
    uint64_t improved[3], worsened[3], equal_changed[3];
    uint64_t changed_tp[3], changed_tn[3], changed_fp[3], changed_fn[3];
    uint64_t saturation[3], resets, action_mismatches, fallback_violations;
    uint64_t residual_false_alarms[3], residual_misses[3];
    uint64_t elapsed_count, elapsed_sum_ms, elapsed_min_ms, elapsed_max_ms;
    uint64_t elapsed_buckets[ELAPSED_BUCKETS];
    uint64_t issued_any_disagreements;
    uint64_t first_difference_ms[3], first_difference_windows[3];
    int offset_min[3], offset_max[3];
    bool first_difference_seen[3];
} Metrics;
typedef struct {
    L1Shadow shadow;
    L1Core action_only, accompanied;
    L1ForecastThresholds thresholds;
    Metrics metrics;
    bool pending;
    uint8_t static_mask, learner_mask, difference_mask;
    uint64_t pending_issued_ms;
    uint64_t last_update_ms[3], windows_at_update[3];
    bool update_waiting[3];
} Diagnostic;
typedef struct {
    uint64_t stream_checksum, schedule_checksum;
    uint64_t schedule_fingerprint[MAX_SEEDS][FAMILY_COUNT];
    Metrics aggregate[2], family[2][FAMILY_COUNT];
    size_t seed_count;
    uint64_t samples;
    uint64_t cpu_ns;
    long rss_kib;
    bool unique_schedules, accounting_ok;
    double scenario_brier_delta[2][MAX_SCENARIOS];
    size_t scenario_count;
} SplitResult;

static uint32_t pcg32_next(Pcg32 *rng) {
    uint64_t old = rng->state;
    rng->state = old * UINT64_C(6364136223846793005) + rng->increment;
    uint32_t x = (uint32_t)(((old >> 18U) ^ old) >> 27U);
    uint32_t r = (uint32_t)(old >> 59U);
    return (x >> r) | (x << ((0U - r) & 31U));
}
static void pcg32_seed(Pcg32 *rng, uint64_t state, uint64_t stream) {
    rng->state = 0; rng->increment = (stream << 1U) | 1U;
    (void)pcg32_next(rng); rng->state += state; (void)pcg32_next(rng);
}
static uint64_t splitmix64(uint64_t *value) {
    uint64_t z = (*value += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31U);
}
static int uniform(Pcg32 *rng, int low, int high) {
    return low + (int)(pcg32_next(rng) % (uint32_t)(high - low + 1));
}
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    return l1_trace_hash_bytes(hash, data, size);
}
static uint64_t hash_integer(uint64_t hash, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        unsigned char byte = (unsigned char)(value >> (8U * i));
        hash = hash_bytes(hash, &byte, 1);
    }
    return hash;
}
static uint64_t ns_between(struct timespec a, struct timespec b) {
    return (uint64_t)(b.tv_sec - a.tv_sec) * UINT64_C(1000000000) +
        (uint64_t)(b.tv_nsec - a.tv_nsec);
}
static uint64_t sat_add(uint64_t value, uint64_t add) {
    return UINT64_MAX - value < add ? UINT64_MAX : value + add;
}
static uint16_t clamp_bp(int value) {
    if (value < 0) return 0;
    if (value > 10000) return 10000;
    return (uint16_t)value;
}

static void add_event(Schedule *schedule, unsigned class_id,
                      uint32_t start, uint32_t duration, size_t length) {
    if (schedule->event_count >= MAX_EVENTS || start >= length - 1U) return;
    if ((uint64_t)start + duration >= length) duration = (uint32_t)length - start - 1U;
    schedule->events[schedule->event_count++] =
        (Event){(uint8_t)class_id, start, duration};
}

static Schedule make_schedule(uint64_t seed, unsigned family) {
    Schedule schedule;
    memset(&schedule, 0, sizeof(schedule));
    uint64_t mix = seed ^ (UINT64_C(0xd6e8feb86659fd93) * (family + 1U));
    Pcg32 truth, fault;
    pcg32_seed(&truth, splitmix64(&mix), splitmix64(&mix));
    pcg32_seed(&fault, splitmix64(&mix), splitmix64(&mix));
    size_t length = family_lengths[family];
    if (family == 1) {
        for (unsigned e = 0; e < 12; ++e)
            add_event(&schedule, pcg32_next(&truth) % 3U,
                      80U + e * 185U + (uint32_t)uniform(&truth, 0, 70),
                      (uint32_t)uniform(&truth, 2, 25), length);
    } else if (family >= 2 && family <= 4) {
        for (unsigned e = 0; e < 6; ++e) {
            unsigned class_id = family - 2U;
            int low = class_id == 0 ? 80 : (class_id == 1 ? 20 : 30);
            int high = class_id == 0 ? 240 : (class_id == 1 ? 180 : 220);
            add_event(&schedule, class_id,
                      180U + e * 440U + (uint32_t)uniform(&truth, 0, 100),
                      (uint32_t)uniform(&truth, low, high), length);
        }
    } else if (family == 5) {
        for (unsigned e = 0; e < 10; ++e) {
            uint32_t start = 180U + e * 310U + (uint32_t)uniform(&truth, 0, 80);
            unsigned first = pcg32_next(&truth) % 3U;
            unsigned second = (first + 1U + pcg32_next(&truth) % 2U) % 3U;
            uint32_t duration = (uint32_t)uniform(&truth, 70, 210);
            add_event(&schedule, first, start, duration, length);
            add_event(&schedule, second, start + duration / 3U,
                      (uint32_t)uniform(&truth, 40, 150), length);
        }
    } else if (family == 6) {
        schedule.regime[0] = (uint32_t)uniform(&truth, 700, 1100);
        schedule.regime[1] = schedule.regime[0] + (uint32_t)uniform(&truth, 900, 1300);
        schedule.regime[2] = schedule.regime[1] + (uint32_t)uniform(&truth, 900, 1300);
        for (unsigned c = 0; c < 3; ++c)
            add_event(&schedule, (pcg32_next(&truth) + c) % 3U,
                      schedule.regime[c], (uint32_t)uniform(&truth, 500, 900), length);
    } else if (family == 7) {
        for (unsigned e = 0; e < 18; ++e)
            add_event(&schedule, (pcg32_next(&truth) + e) % 3U,
                      120U + e * 285U + (uint32_t)uniform(&truth, 0, 80),
                      (uint32_t)uniform(&truth, 50, 180), length);
    } else if (family == 8 || family == 9) {
        for (unsigned e = 0; e < 8; ++e)
            add_event(&schedule, pcg32_next(&truth) % 3U,
                      150U + e * 340U + (uint32_t)uniform(&truth, 0, 70),
                      (uint32_t)uniform(&truth, 40, 170), length);
        schedule.fault_count = family == 8 ? 4U : 6U;
        for (unsigned f = 0; f < schedule.fault_count; ++f)
            schedule.faults[f] = 300U + f * 420U +
                (uint32_t)uniform(&fault, 0, 120);
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_integer(hash, family, 4);
    hash = hash_integer(hash, schedule.event_count, 4);
    for (unsigned e = 0; e < schedule.event_count; ++e) {
        hash = hash_integer(hash, schedule.events[e].class_id, 1);
        hash = hash_integer(hash, schedule.events[e].start, 4);
        hash = hash_integer(hash, schedule.events[e].duration, 4);
    }
    for (unsigned r = 0; r < 3; ++r)
        hash = hash_integer(hash, schedule.regime[r], 4);
    hash = hash_integer(hash, schedule.fault_count, 4);
    for (unsigned f = 0; f < schedule.fault_count; ++f)
        hash = hash_integer(hash, schedule.faults[f], 4);
    schedule.fingerprint = hash;
    return schedule;
}

static uint8_t latent_at(const Schedule *schedule, size_t index) {
    uint8_t mask = 0;
    for (unsigned e = 0; e < schedule->event_count; ++e) {
        uint64_t end = (uint64_t)schedule->events[e].start +
            schedule->events[e].duration;
        if (index >= schedule->events[e].start && index < end)
            mask |= (uint8_t)(1U << schedule->events[e].class_id);
    }
    return mask;
}

static void metrics_init(Metrics *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    metrics->elapsed_min_ms = UINT64_MAX;
    for (unsigned c = 0; c < 3; ++c) {
        metrics->offset_min[c] = 0;
        metrics->offset_max[c] = 0;
        metrics->first_difference_ms[c] = UINT64_MAX;
        metrics->first_difference_windows[c] = UINT64_MAX;
    }
}
static void diagnostic_init(Diagnostic *diagnostic, L1ForecastThresholds thresholds) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->thresholds = thresholds;
    l1_shadow_init(&diagnostic->shadow, thresholds);
    l1_init(&diagnostic->action_only);
    l1_init(&diagnostic->accompanied);
    L1Policy policy = {1, 1, 2000, 50, 300, 2};
    if (!l1_apply_policy(&diagnostic->action_only, &policy) ||
        !l1_apply_policy(&diagnostic->accompanied, &policy)) abort();
    metrics_init(&diagnostic->metrics);
}
static uint16_t observed_for(L1Observation observation, unsigned c) {
    return c == 0 ? observation.cpu_some_bp :
        (c == 1 ? observation.memory_full_bp : observation.io_full_bp);
}
static uint16_t threshold_for(L1ForecastThresholds thresholds, unsigned c) {
    return c == 0 ? thresholds.cpu_bp : (c == 1 ? thresholds.memory_bp : thresholds.io_bp);
}
static uint64_t counter_for(L1PressureCounters counters, unsigned c) {
    return c == 0 ? counters.cpu_some_total_us :
        (c == 1 ? counters.memory_full_total_us : counters.io_full_total_us);
}
static void score(bool prediction, bool outcome, uint64_t *tp, uint64_t *tn,
                  uint64_t *fp, uint64_t *fn) {
    if (prediction && outcome) (*tp)++;
    else if (!prediction && !outcome) (*tn)++;
    else if (prediction) (*fp)++;
    else (*fn)++;
}

static bool adaptation_gate_ready(const L1Shadow *before, unsigned c,
                                  uint64_t now_ms, uint16_t evaluated) {
    uint32_t valid = before->valid_samples[c];
    if (valid < UINT32_MAX) valid++;
    return valid >= L1_SELF_ADAPT_MIN_SAMPLES && before->gate_started[c] &&
        now_ms - before->gate_started_ms[c] >= L1_SELF_ADAPT_MIN_INTERVAL_MS &&
        evaluated >= L1_FORECAST_MIN_WINDOWS;
}

static void count_saturation_attempts(Diagnostic *d, const L1Shadow *before,
                                      const L1Feedback *feedback,
                                      uint64_t now_ms) {
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        bool prediction = (before->pending_prediction.positive_mask &
                           (uint8_t)(1U << c)) != 0;
        bool outcome = (feedback->positive_mask & (uint8_t)(1U << c)) != 0;
        uint16_t false_alarms = before->false_alarms[c];
        uint16_t misses = before->misses[c];
        uint16_t evaluated = before->evaluated_windows[c];
        if (prediction && !outcome && false_alarms < UINT16_MAX) false_alarms++;
        if (!prediction && outcome && misses < UINT16_MAX) misses++;
        if (evaluated < UINT16_MAX) evaluated++;
        if (!adaptation_gate_ready(before, c, now_ms, evaluated)) continue;
        if ((before->offset[c] == L1_FORECAST_MAX_OFFSET &&
             (uint32_t)false_alarms >= (uint32_t)misses + 2U) ||
            (before->offset[c] == -L1_FORECAST_MAX_OFFSET &&
             (uint32_t)misses >= (uint32_t)false_alarms + 2U))
            d->metrics.saturation[c]++;
    }
}

static void classify_discard(Diagnostic *d, const L1Shadow *before,
                             L1Observation observation, L1PressureCounters counters,
                             uint64_t now_ms) {
    if (!before->pending) return;
    if (!observation.valid || !counters.valid) { d->metrics.invalid++; return; }
    if (before->monotonic_initialized && now_ms < before->last_monotonic_ms) {
        d->metrics.regression++; return;
    }
    if (before->monotonic_initialized) {
        uint64_t elapsed = now_ms - before->last_monotonic_ms;
        bool decrease = false, impossible = elapsed > UINT64_MAX / 1000U;
        uint64_t elapsed_us = impossible ? 0 : elapsed * 1000U;
        for (unsigned c = 0; c < 3; ++c) {
            uint64_t now = counter_for(counters, c);
            uint64_t prior = counter_for(before->last_counters, c);
            if (now < prior) decrease = true;
            else if (!impossible && now - prior > elapsed_us) impossible = true;
        }
        if (decrease) { d->metrics.reset++; return; }
        if (impossible) { d->metrics.impossible++; return; }
        if (elapsed > L1_FORECAST_MAX_GAP_MS ||
            now_ms - before->pending_started_ms > L1_FORECAST_LATEST_MS) {
            d->metrics.gap_late++; return;
        }
    }
}

static void diagnostic_step(Diagnostic *d, L1Observation observation,
                            L1PressureCounters counters, uint64_t now_ms) {
    d->metrics.observations++;
    bool core_regression = d->action_only.monotonic_initialized &&
        now_ms < d->action_only.last_monotonic_ms;
    L1Action a = l1_step_at(&d->action_only, observation, now_ms);
    L1Action b = l1_step_at(&d->accompanied, observation, now_ms);
    if (a != b || memcmp(&d->action_only, &d->accompanied, sizeof(L1Core)) != 0)
        d->metrics.action_mismatches++;
    if ((!observation.valid || core_regression) && a != L1_HOLD)
        d->metrics.fallback_violations++;
    L1Shadow before = d->shadow;
    L1ShadowStep step = l1_shadow_step_at(&d->shadow, observation, counters, now_ms);
    if (d->shadow.discarded_invalid > before.discarded_invalid ||
        d->shadow.discarded_gap_or_late > before.discarded_gap_or_late ||
        d->shadow.discarded_reset > before.discarded_reset) {
        classify_discard(d, &before, observation, counters, now_ms);
        d->pending = false;
    }
    if (d->shadow.reset_count > before.reset_count)
        d->metrics.resets += d->shadow.reset_count - before.reset_count;
    if (step.feedback_ready && d->pending) {
        uint64_t elapsed = now_ms - d->pending_issued_ms;
        d->metrics.elapsed_count++;
        d->metrics.elapsed_sum_ms += elapsed;
        if (elapsed < d->metrics.elapsed_min_ms) d->metrics.elapsed_min_ms = elapsed;
        if (elapsed > d->metrics.elapsed_max_ms) d->metrics.elapsed_max_ms = elapsed;
        unsigned bucket = elapsed <= L1_FORECAST_HORIZON_MS ? 0U :
            (unsigned)((elapsed - L1_FORECAST_HORIZON_MS + 999U) / 1000U);
        if (bucket >= ELAPSED_BUCKETS) bucket = ELAPSED_BUCKETS - 1U;
        d->metrics.elapsed_buckets[bucket]++;
        count_saturation_attempts(d, &before, &step.feedback, now_ms);
        d->metrics.evaluated++;
        for (unsigned c = 0; c < 3; ++c) {
            bool outcome = (step.feedback.positive_mask & (1U << c)) != 0;
            bool fixed = (d->static_mask & (1U << c)) != 0;
            bool learned = (d->learner_mask & (1U << c)) != 0;
            score(fixed, outcome, &d->metrics.static_tp[c], &d->metrics.static_tn[c],
                  &d->metrics.static_fp[c], &d->metrics.static_fn[c]);
            score(learned, outcome, &d->metrics.learner_tp[c], &d->metrics.learner_tn[c],
                  &d->metrics.learner_fp[c], &d->metrics.learner_fn[c]);
            if (d->difference_mask & (1U << c)) {
                d->metrics.disagreements[c]++;
                bool fixed_correct = fixed == outcome, learned_correct = learned == outcome;
                if (learned_correct && !fixed_correct) d->metrics.improved[c]++;
                else if (!learned_correct && fixed_correct) d->metrics.worsened[c]++;
                else d->metrics.equal_changed[c]++;
                score(learned, outcome, &d->metrics.changed_tp[c],
                      &d->metrics.changed_tn[c], &d->metrics.changed_fp[c],
                      &d->metrics.changed_fn[c]);
                if (d->update_waiting[c] && !d->metrics.first_difference_seen[c]) {
                    d->metrics.first_difference_seen[c] = true;
                    d->metrics.first_difference_ms[c] =
                        d->pending_issued_ms - d->last_update_ms[c];
                    d->metrics.first_difference_windows[c] =
                        d->metrics.evaluated - d->windows_at_update[c];
                }
            }
        }
        d->pending = false;
    }
    for (unsigned c = 0; c < 3; ++c) {
        if (d->shadow.updates[c] > before.updates[c]) {
            d->last_update_ms[c] = now_ms;
            d->windows_at_update[c] = d->metrics.evaluated;
            d->update_waiting[c] = true;
        }
        if (d->shadow.offset[c] < d->metrics.offset_min[c])
            d->metrics.offset_min[c] = d->shadow.offset[c];
        if (d->shadow.offset[c] > d->metrics.offset_max[c])
            d->metrics.offset_max[c] = d->shadow.offset[c];
        d->metrics.updates[c] = d->shadow.updates[c];
    }
    if (step.prediction_issued) {
        L1Prediction fixed;
        if (!l1_fixed_prediction(observation, d->thresholds, &fixed)) return;
        d->metrics.issued++;
        d->pending = true;
        d->static_mask = fixed.positive_mask;
        d->learner_mask = step.prediction.positive_mask;
        d->difference_mask = fixed.positive_mask ^ step.prediction.positive_mask;
        if (d->difference_mask != 0) d->metrics.issued_any_disagreements++;
        d->pending_issued_ms = now_ms;
        for (unsigned c = 0; c < 3; ++c) {
            if (d->shadow.offset[c] != 0) d->metrics.offset_windows[c]++;
            uint16_t observed = observed_for(observation, c);
            uint16_t base = threshold_for(d->thresholds, c);
            uint16_t effective = step.prediction.effective_threshold_bp[c];
            if ((observed >= base) != (observed >= effective)) d->metrics.boundary[c]++;
            if (d->difference_mask & (1U << c))
                d->metrics.issued_disagreements[c]++;
        }
    }
}

static void metrics_finish(Diagnostic *diagnostic) {
    if (diagnostic->pending) diagnostic->metrics.unfinished++;
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        diagnostic->metrics.residual_false_alarms[c] =
            diagnostic->shadow.false_alarms[c];
        diagnostic->metrics.residual_misses[c] = diagnostic->shadow.misses[c];
    }
}
static void metrics_add(Metrics *to, const Metrics *from) {
#define ADD_FIELD(name) to->name += from->name
    ADD_FIELD(issued); ADD_FIELD(evaluated); ADD_FIELD(invalid); ADD_FIELD(gap_late);
    ADD_FIELD(regression); ADD_FIELD(reset); ADD_FIELD(impossible);
    ADD_FIELD(unfinished); ADD_FIELD(observations); ADD_FIELD(resets);
    ADD_FIELD(action_mismatches); ADD_FIELD(fallback_violations);
    ADD_FIELD(elapsed_count); ADD_FIELD(elapsed_sum_ms);
    ADD_FIELD(issued_any_disagreements);
    if (from->elapsed_count) {
        if (from->elapsed_min_ms < to->elapsed_min_ms)
            to->elapsed_min_ms = from->elapsed_min_ms;
        if (from->elapsed_max_ms > to->elapsed_max_ms)
            to->elapsed_max_ms = from->elapsed_max_ms;
    }
    for (unsigned b = 0; b < ELAPSED_BUCKETS; ++b)
        to->elapsed_buckets[b] += from->elapsed_buckets[b];
    for (unsigned c = 0; c < 3; ++c) {
        to->static_tp[c] += from->static_tp[c]; to->static_tn[c] += from->static_tn[c];
        to->static_fp[c] += from->static_fp[c]; to->static_fn[c] += from->static_fn[c];
        to->learner_tp[c] += from->learner_tp[c]; to->learner_tn[c] += from->learner_tn[c];
        to->learner_fp[c] += from->learner_fp[c]; to->learner_fn[c] += from->learner_fn[c];
        to->updates[c] += from->updates[c]; to->offset_windows[c] += from->offset_windows[c];
        to->boundary[c] += from->boundary[c];
        to->issued_disagreements[c] += from->issued_disagreements[c];
        to->disagreements[c] += from->disagreements[c];
        to->improved[c] += from->improved[c]; to->worsened[c] += from->worsened[c];
        to->equal_changed[c] += from->equal_changed[c];
        to->changed_tp[c] += from->changed_tp[c]; to->changed_tn[c] += from->changed_tn[c];
        to->changed_fp[c] += from->changed_fp[c]; to->changed_fn[c] += from->changed_fn[c];
        to->saturation[c] += from->saturation[c];
        to->residual_false_alarms[c] += from->residual_false_alarms[c];
        to->residual_misses[c] += from->residual_misses[c];
        if (from->offset_min[c] < to->offset_min[c]) to->offset_min[c] = from->offset_min[c];
        if (from->offset_max[c] > to->offset_max[c]) to->offset_max[c] = from->offset_max[c];
        if (from->first_difference_seen[c] &&
            (!to->first_difference_seen[c] ||
             from->first_difference_ms[c] < to->first_difference_ms[c])) {
            to->first_difference_seen[c] = true;
            to->first_difference_ms[c] = from->first_difference_ms[c];
            to->first_difference_windows[c] = from->first_difference_windows[c];
        }
    }
#undef ADD_FIELD
}

static bool metrics_account(const Metrics *m) {
    if (m->issued != m->evaluated + m->invalid + m->gap_late +
        m->regression + m->reset + m->impossible + m->unfinished) return false;
    if (m->elapsed_count != m->evaluated) return false;
    for (unsigned c = 0; c < 3; ++c)
        if (m->disagreements[c] != m->improved[c] + m->worsened[c] +
            m->equal_changed[c]) return false;
    return true;
}

static double macro_brier(const Metrics *m, bool learner) {
    double total = 0.0;
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        uint64_t fp = learner ? m->learner_fp[c] : m->static_fp[c];
        uint64_t fn = learner ? m->learner_fn[c] : m->static_fn[c];
        uint64_t n = learner ? m->learner_tp[c] + m->learner_tn[c] + fp + fn :
            m->static_tp[c] + m->static_tn[c] + fp + fn;
        if (!n) return NAN;
        total += (double)(fp + fn) / (double)n;
    }
    return total / (double)CLASS_COUNT;
}

static void scenario_run(uint64_t seed, unsigned family,
                         L1ForecastThresholds thresholds, Metrics *metrics,
                         uint64_t *stream_checksum, uint64_t *schedule_fingerprint) {
    Schedule schedule = make_schedule(seed, family);
    *schedule_fingerprint = schedule.fingerprint;
    uint64_t mix = seed ^ (UINT64_C(0xa0761d6478bd642f) * (family + 1U));
    Pcg32 noise, outcome, timing;
    pcg32_seed(&noise, splitmix64(&mix), splitmix64(&mix));
    pcg32_seed(&outcome, splitmix64(&mix), splitmix64(&mix));
    pcg32_seed(&timing, splitmix64(&mix), splitmix64(&mix));
    Diagnostic diagnostic;
    diagnostic_init(&diagnostic, thresholds);
    uint64_t totals[3] = {0}, timestamp = 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < family_lengths[family]; ++i) {
        uint64_t increment = 1000;
        bool invalid = false;
        if (family == 8) {
            for (unsigned f = 0; f < schedule.fault_count; ++f) {
                if (i >= schedule.faults[f] && i < schedule.faults[f] + 8U)
                    invalid = true;
                if (i == schedule.faults[f] + 20U)
                    increment = (uint64_t)uniform(&timing, 6000, 12000);
            }
        }
        if (i > 0) timestamp += increment;
        if (family == 9) {
            if (i == schedule.faults[0] || i == schedule.faults[3])
                timestamp -= timestamp >= 4000 ? 4000 : timestamp;
            if (i == schedule.faults[1] || i == schedule.faults[4])
                totals[0] = totals[1] = totals[2] = 10;
        }
        uint8_t latent = latent_at(&schedule, i);
        int nominal[3] = {
            (latent & 1U) ? 3600 : 600,
            (latent & 2U) ? 180 : 10,
            (latent & 4U) ? 750 : 50
        };
        if (family == 7) {
            bool trained_phase = ((i / 600U) & 1U) != 0;
            const uint16_t *base = trained_phase ?
                (const uint16_t[]){2000, 75, 600} :
                (const uint16_t[]){800, 40, 150};
            for (unsigned c = 0; c < 3; ++c) {
                int direction = (latent & (1U << c)) ? -1 : 1;
                int step = c == 0 ? 200 : (c == 1 ? 10 : 40);
                nominal[c] = (int)base[c] + direction * step / 2;
            }
        }
        L1Observation observation = {
            clamp_bp(nominal[0] + uniform(&noise, -400, 400)),
            clamp_bp(nominal[1] + uniform(&noise, -30, 30)),
            clamp_bp(nominal[2] + uniform(&noise, -120, 120)), !invalid
        };
        int active_rate[3] = {1200, 80, 300};
        int quiet_rate[3] = {100, 2, 10};
        for (unsigned c = 0; c < 3; ++c) {
            int rate = (latent & (1U << c)) ? active_rate[c] : quiet_rate[c];
            rate += uniform(&outcome, -(rate / 8 + 1), rate / 8 + 1);
            if (rate < 0) rate = 0;
            totals[c] = sat_add(totals[c], (uint64_t)rate * increment * 1000U / 10000U);
        }
        if (family == 9 && (i == schedule.faults[2] || i == schedule.faults[5]))
            totals[2] = sat_add(totals[2], increment * 2000U);
        L1PressureCounters counters = {totals[0], totals[1], totals[2], !invalid};
        if (invalid) {
            observation = (L1Observation){0, 0, 0, false};
            counters = (L1PressureCounters){0, 0, 0, false};
        }
        hash = hash_integer(hash, timestamp, 8);
        hash = hash_integer(hash, observation.cpu_some_bp, 2);
        hash = hash_integer(hash, observation.memory_full_bp, 2);
        hash = hash_integer(hash, observation.io_full_bp, 2);
        hash = hash_integer(hash, observation.valid ? 1U : 0U, 1);
        hash = hash_integer(hash, counters.cpu_some_total_us, 8);
        hash = hash_integer(hash, counters.memory_full_total_us, 8);
        hash = hash_integer(hash, counters.io_full_total_us, 8);
        hash = hash_integer(hash, counters.valid ? 1U : 0U, 1);
        hash = hash_integer(hash, latent, 1);
        diagnostic_step(&diagnostic, observation, counters, timestamp);
    }
    metrics_finish(&diagnostic);
    *metrics = diagnostic.metrics;
    *stream_checksum = hash;
}

static SplitResult run_split(const uint64_t *seeds, size_t seed_count,
                             unsigned repetitions) {
    SplitResult result;
    memset(&result, 0, sizeof(result));
    result.stream_checksum = UINT64_C(1469598103934665603);
    result.schedule_checksum = UINT64_C(1469598103934665603);
    result.seed_count = seed_count;
    result.unique_schedules = true;
    result.accounting_ok = true;
    for (unsigned c = 0; c < 2; ++c) {
        metrics_init(&result.aggregate[c]);
        for (unsigned f = 0; f < FAMILY_COUNT; ++f)
            metrics_init(&result.family[c][f]);
    }
    struct timespec begin, end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
    for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
        for (size_t s = 0; s < seed_count; ++s) {
            for (unsigned f = 0; f < FAMILY_COUNT; ++f) {
                uint64_t scenario_stream = 0, schedule = 0;
                Metrics scenario_metrics[2];
                for (unsigned config = 0; config < 2; ++config) {
                    Metrics one;
                    scenario_run(seeds[s], f, configurations[config], &one,
                                 &scenario_stream, &schedule);
                    if (repetition == 0) {
                        scenario_metrics[config] = one;
                        metrics_add(&result.aggregate[config], &one);
                        metrics_add(&result.family[config][f], &one);
                        result.accounting_ok = result.accounting_ok && metrics_account(&one);
                    }
                }
                if (repetition == 0) {
                    size_t scenario_index = s * FAMILY_COUNT + f;
                    for (unsigned config = 0; config < 2; ++config)
                        result.scenario_brier_delta[config][scenario_index] =
                            macro_brier(&scenario_metrics[config], true) -
                            macro_brier(&scenario_metrics[config], false);
                    result.schedule_fingerprint[s][f] = schedule;
                    result.stream_checksum = hash_integer(result.stream_checksum,
                                                          scenario_stream, 8);
                    result.schedule_checksum = hash_integer(result.schedule_checksum,
                                                            schedule, 8);
                    result.samples += family_lengths[f] * 2U;
                    result.scenario_count++;
                }
            }
        }
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    result.cpu_ns = ns_between(begin, end);
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    result.rss_kib = usage.ru_maxrss;
    for (unsigned f = 1; f < FAMILY_COUNT; ++f)
        for (size_t a = 0; a < seed_count; ++a)
            for (size_t b = a + 1; b < seed_count; ++b)
                if (result.schedule_fingerprint[a][f] ==
                    result.schedule_fingerprint[b][f])
                    result.unique_schedules = false;
    return result;
}

static double ratio(uint64_t numerator, uint64_t denominator) {
    return denominator ? (double)numerator / (double)denominator : NAN;
}
static void ratio_text(char *buffer, size_t size, uint64_t numerator,
                       uint64_t denominator) {
    if (!denominator) snprintf(buffer, size, "unavailable");
    else snprintf(buffer, size, "%.6f", (double)numerator / (double)denominator);
}
static uint64_t elapsed_p95(const Metrics *m) {
    if (!m->elapsed_count) return 0;
    uint64_t target = (m->elapsed_count * 95U + 99U) / 100U, cumulative = 0;
    for (unsigned b = 0; b < ELAPSED_BUCKETS; ++b) {
        cumulative += m->elapsed_buckets[b];
        if (cumulative >= target) return L1_FORECAST_HORIZON_MS + b * 1000U;
    }
    return m->elapsed_max_ms;
}
static void print_metrics(const char *scope, const char *config, const Metrics *m) {
    char eligibility[32];
    ratio_text(eligibility, sizeof(eligibility), m->evaluated, m->issued);
    printf("DIAGNOSTIC,%s,%s,observations=%" PRIu64 ",issued=%" PRIu64
           ",evaluated=%" PRIu64 ",invalid=%" PRIu64 ",gap_late=%" PRIu64
           ",regression=%" PRIu64 ",reset=%" PRIu64 ",impossible=%" PRIu64
           ",unfinished=%" PRIu64 ",eligibility=%s,action_mismatches=%" PRIu64
           ",fallback_violations=%" PRIu64 "\n", scope, config, m->observations,
           m->issued, m->evaluated, m->invalid, m->gap_late, m->regression,
           m->reset, m->impossible, m->unfinished, eligibility,
           m->action_mismatches, m->fallback_violations);
    if (m->elapsed_count)
        printf("WINDOW_ELAPSED,%s,%s,count=%" PRIu64 ",min_ms=%" PRIu64
               ",mean_ms=%.3f,p95_ms=%" PRIu64 ",max_ms=%" PRIu64 "\n",
               scope, config, m->elapsed_count, m->elapsed_min_ms,
               (double)m->elapsed_sum_ms / (double)m->elapsed_count,
               elapsed_p95(m), m->elapsed_max_ms);
    else
        printf("WINDOW_ELAPSED,%s,%s,count=0,min_ms=unavailable,mean_ms=unavailable"
               ",p95_ms=unavailable,max_ms=unavailable\n", scope, config);
    printf("ANY_CLASS,%s,%s,issued_disagreements=%" PRIu64 "\n", scope,
           config, m->issued_any_disagreements);
    for (unsigned c = 0; c < 3; ++c) {
        uint64_t static_n = m->static_tp[c] + m->static_tn[c] +
            m->static_fp[c] + m->static_fn[c];
        uint64_t learner_n = m->learner_tp[c] + m->learner_tn[c] +
            m->learner_fp[c] + m->learner_fn[c];
        char sp[32], sr[32], sfar[32], sf1[32], sbrier[32], sprevalence[32];
        char lp[32], lr[32], lfar[32], lf1[32], lbrier[32], lprevalence[32];
        ratio_text(sp, sizeof(sp), m->static_tp[c], m->static_tp[c] + m->static_fp[c]);
        ratio_text(sr, sizeof(sr), m->static_tp[c], m->static_tp[c] + m->static_fn[c]);
        ratio_text(sfar, sizeof(sfar), m->static_fp[c], m->static_fp[c] + m->static_tn[c]);
        ratio_text(sf1, sizeof(sf1), 2U * m->static_tp[c],
                   2U * m->static_tp[c] + m->static_fp[c] + m->static_fn[c]);
        ratio_text(sbrier, sizeof(sbrier), m->static_fp[c] + m->static_fn[c], static_n);
        ratio_text(sprevalence, sizeof(sprevalence), m->static_tp[c] + m->static_fn[c], static_n);
        ratio_text(lp, sizeof(lp), m->learner_tp[c], m->learner_tp[c] + m->learner_fp[c]);
        ratio_text(lr, sizeof(lr), m->learner_tp[c], m->learner_tp[c] + m->learner_fn[c]);
        ratio_text(lfar, sizeof(lfar), m->learner_fp[c], m->learner_fp[c] + m->learner_tn[c]);
        ratio_text(lf1, sizeof(lf1), 2U * m->learner_tp[c],
                   2U * m->learner_tp[c] + m->learner_fp[c] + m->learner_fn[c]);
        ratio_text(lbrier, sizeof(lbrier), m->learner_fp[c] + m->learner_fn[c], learner_n);
        ratio_text(lprevalence, sizeof(lprevalence), m->learner_tp[c] + m->learner_fn[c], learner_n);
        printf("CLASS,%s,%s,%s,static=%" PRIu64 "/%" PRIu64 "/%" PRIu64
               "/%" PRIu64 ",learner=%" PRIu64 "/%" PRIu64 "/%" PRIu64
               "/%" PRIu64 ",updates=%" PRIu64 ",offset=%d..%d"
               ",offset_windows=%" PRIu64 ",boundary=%" PRIu64
               ",issued_disagreements=%" PRIu64 ",scored_disagreements=%" PRIu64
               ",improved=%" PRIu64
               ",worsened=%" PRIu64 ",equal=%" PRIu64
               ",changed=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
               ",saturation_attempts=%" PRIu64
               ",residual_false_alarm_evidence=%" PRIu64
               ",residual_miss_evidence=%" PRIu64
               ",first_difference_ms=%s,first_difference_windows=%s\n",
               scope, config, class_names[c], m->static_tp[c], m->static_tn[c],
               m->static_fp[c], m->static_fn[c], m->learner_tp[c],
               m->learner_tn[c], m->learner_fp[c], m->learner_fn[c],
               m->updates[c], m->offset_min[c], m->offset_max[c],
               m->offset_windows[c], m->boundary[c], m->issued_disagreements[c],
               m->disagreements[c],
               m->improved[c], m->worsened[c], m->equal_changed[c],
               m->changed_tp[c], m->changed_tn[c], m->changed_fp[c],
               m->changed_fn[c], m->saturation[c],
               m->residual_false_alarms[c], m->residual_misses[c],
               m->first_difference_seen[c] ? "reported" : "none",
               m->first_difference_seen[c] ? "reported" : "none");
        printf("SCORES,%s,%s,%s,static_precision=%s,static_recall=%s"
               ",static_false_alarm_rate=%s,static_f1=%s,static_brier=%s"
               ",static_prevalence=%s,learner_precision=%s,learner_recall=%s"
               ",learner_false_alarm_rate=%s,learner_f1=%s,learner_brier=%s"
               ",learner_prevalence=%s\n", scope, config, class_names[c],
               sp, sr, sfar, sf1, sbrier, sprevalence,
               lp, lr, lfar, lf1, lbrier, lprevalence);
        if (m->first_difference_seen[c])
            printf("FIRST_DIFFERENCE,%s,%s,%s,ms=%" PRIu64 ",windows=%" PRIu64 "\n",
                   scope, config, class_names[c], m->first_difference_ms[c],
                   m->first_difference_windows[c]);
    }
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static double exact_two_sided_sign_p(unsigned wins, unsigned losses) {
    unsigned n = wins + losses, tail = wins < losses ? wins : losses;
    if (!n) return 1.0;
    double probability = ldexp(1.0, -(int)n), sum = probability;
    for (unsigned k = 1; k <= tail; ++k) {
        probability *= (double)(n - k + 1U) / (double)k;
        sum += probability;
    }
    sum *= 2.0;
    return sum > 1.0 ? 1.0 : sum;
}

static void print_uncertainty(const char *split, const char *config,
                              const double *values, size_t count,
                              const uint64_t *seeds, size_t seed_count) {
    double sorted[MAX_SCENARIOS], sum = 0.0;
    size_t n = 0;
    unsigned wins = 0, losses = 0, ties = 0;
    for (size_t i = 0; i < count; ++i) {
        if (isnan(values[i])) continue;
        sorted[n++] = values[i];
        sum += values[i];
        if (values[i] < 0.0) wins++;
        else if (values[i] > 0.0) losses++;
        else ties++;
    }
    if (!n) {
        printf("UNCERTAINTY,%s,%s,available=no\n", split, config);
        return;
    }
    qsort(sorted, n, sizeof(*sorted), compare_double);
    double mean = sum / (double)n, squares = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double difference = sorted[i] - mean;
        squares += difference * difference;
    }
    double standard_deviation = n > 1 ? sqrt(squares / (double)(n - 1U)) : 0.0;
    double median = n & 1U ? sorted[n / 2U] :
        (sorted[n / 2U - 1U] + sorted[n / 2U]) / 2.0;
    printf("UNCERTAINTY,%s,%s,metric=learner_minus_static_macro_brier"
           ",schedules=%zu,mean=%.9f,sample_sd=%.9f,median=%.9f"
           ",min=%.9f,max=%.9f,wins=%u,losses=%u,ties=%u"
           ",exact_two_sided_sign_p=%.9f\n", split, config, n, mean,
           standard_deviation, median, sorted[0], sorted[n - 1U], wins,
           losses, ties, exact_two_sided_sign_p(wins, losses));
    for (size_t s = 0; s < seed_count; ++s) {
        double seed_sum = 0.0;
        size_t seed_n = 0;
        for (unsigned f = 0; f < FAMILY_COUNT; ++f) {
            double value = values[s * FAMILY_COUNT + f];
            if (!isnan(value)) { seed_sum += value; seed_n++; }
        }
        printf("SEED_AGGREGATE,%s,%s,seed=%" PRIu64 ",scenario_count=%zu"
               ",learner_minus_static_macro_brier=%s",
               split, config, seeds[s], seed_n,
               seed_n ? "reported" : "unavailable");
        if (seed_n) printf(",value=%.9f", seed_sum / (double)seed_n);
        putchar('\n');
    }
}

static bool frozen_matches(const char *mode, const SplitResult *result) {
    uint64_t stream = 0, schedule = 0;
    if (!strcmp(mode, "development")) {
        stream = frozen_development_stream; schedule = frozen_development_schedule;
    } else if (!strcmp(mode, "verification")) {
        stream = frozen_verification_stream; schedule = frozen_verification_schedule;
    } else {
        stream = frozen_heldout_stream; schedule = frozen_heldout_schedule;
    }
    return stream != 0 && schedule != 0 &&
        result->stream_checksum == stream && result->schedule_checksum == schedule;
}

static int synthetic_mode(const char *mode, unsigned repetitions, bool identity_only) {
    const uint64_t *seeds;
    size_t count;
    if (!strcmp(mode, "development")) {
        seeds = development_seeds; count = sizeof(development_seeds) / sizeof(*seeds);
    } else if (!strcmp(mode, "verification")) {
        seeds = verification_seeds; count = sizeof(verification_seeds) / sizeof(*seeds);
    } else {
        seeds = heldout_seeds; count = sizeof(heldout_seeds) / sizeof(*seeds);
    }
    SplitResult result = run_split(seeds, count, repetitions);
    printf("V07_IDENTITY,split=%s,seeds=%zu,scenarios=%zu,stream=%016" PRIx64
           ",schedule=%016" PRIx64 ",unique=%s\n", mode, count,
           count * FAMILY_COUNT, result.stream_checksum, result.schedule_checksum,
           result.unique_schedules ? "PASS" : "FAIL");
    if (identity_only) return result.unique_schedules ? 0 : 2;
    if (!frozen_matches(mode, &result)) {
        fputs("V0.7 split identity is not pinned or does not match\n", stderr);
        return 2;
    }
    for (unsigned config = 0; config < 2; ++config) {
        for (unsigned f = 0; f < FAMILY_COUNT; ++f)
            print_metrics(family_names[f], configuration_names[config],
                          &result.family[config][f]);
        print_metrics("ALL", configuration_names[config], &result.aggregate[config]);
        print_uncertainty(mode, configuration_names[config],
                          result.scenario_brier_delta[config],
                          result.scenario_count, seeds, result.seed_count);
    }
    double ns_per_sample = result.samples && repetitions ?
        (double)result.cpu_ns / (double)(result.samples * repetitions) : NAN;
    bool exercised = true, safe = result.accounting_ok && result.unique_schedules;
    for (unsigned config = 0; config < 2; ++config) {
        Metrics *m = &result.aggregate[config];
        safe = safe && !m->action_mismatches && !m->fallback_violations &&
            ratio(m->evaluated, m->issued) >= 0.80;
        for (unsigned c = 0; c < 3; ++c)
            exercised = exercised && m->updates[c] && m->offset_windows[c] &&
                m->boundary[c] && m->disagreements[c];
    }
    bool budgets = result.rss_kib <= 16384 && sizeof(L1Core) == 104 &&
        sizeof(L1Shadow) <= 208 && ns_per_sample <= 500000.0;
    printf("V07_RESOURCE,split=%s,samples=%" PRIu64 ",repetitions=%u"
           ",cpu_ns_per_sample=%.3f,rss_kib=%ld,sizeof_l1core=%zu"
           ",sizeof_l1shadow=%zu,explicit_schedule_bytes=%zu"
           ",explicit_metrics_bytes=%zu,budgets=%s\n", mode, result.samples,
           repetitions, ns_per_sample, result.rss_kib, sizeof(L1Core),
           sizeof(L1Shadow), sizeof(Schedule),
           sizeof(result.aggregate) + sizeof(result.family),
           budgets ? "PASS" : "FAIL");
    if (!strcmp(mode, "heldout")) {
        bool go = safe && exercised && budgets;
        printf("DIAGNOSTICS_%s\nSHADOW_PREDICTION,NO_GO\nACTION_COUPLING,NO_GO\n",
               go ? "GO" : "NO_GO");
    }
    return safe ? 0 : 2;
}

typedef struct { Diagnostic diagnostic; } TraceContext;
static bool trace_row(const L1TraceRow *row, void *context) {
    TraceContext *trace = context;
    diagnostic_step(&trace->diagnostic, row->observation, row->counters,
                    row->relative_ms);
    return true;
}
static int trace_mode(const char *path, const char *name) {
    unsigned config;
    if (!strcmp(name, "live-default")) config = 0;
    else if (!strcmp(name, "v06-trained")) config = 1;
    else return 2;
    TraceContext context;
    diagnostic_init(&context.diagnostic, configurations[config]);
    L1TraceInfo info;
    char error[160] = {0};
    if (!l1_trace_read(path, trace_row, &context, &info, error, sizeof(error))) {
        fprintf(stderr, "trace rejected: %s\n", error);
        return 2;
    }
    metrics_finish(&context.diagnostic);
    print_metrics("REAL_TRACE", name, &context.diagnostic.metrics);
    printf("TRACE_PROVENANCE,schema=1,samples=%" PRIu64 ",bytes=%" PRIu64
           ",checksum=%016" PRIx64 ",horizon_eligible=%s,latent_truth=absent\n",
           info.count, info.bytes, info.checksum,
           info.horizon_eligible ? "yes" : "no");
    return metrics_account(&context.diagnostic.metrics) ? 0 : 2;
}

static bool parse_repetitions(const char *text, unsigned *out) {
    if (text == NULL || *text == '\0') return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (unsigned)(*p - '0');
        if (value > 100) return false;
    }
    if (value < 1) return false;
    *out = value;
    return true;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "trace"))
        return trace_mode(argv[2], argv[3]);
    if (argc >= 2 && argc <= 3) {
        unsigned repetitions = 1;
        if (argc == 3 && !parse_repetitions(argv[2], &repetitions)) return 2;
        if (!strcmp(argv[1], "development"))
            return synthetic_mode("development", repetitions, false);
        if (!strcmp(argv[1], "verification"))
            return synthetic_mode("verification", repetitions, false);
        if (!strcmp(argv[1], "identity-development"))
            return synthetic_mode("development", 1, true);
        if (!strcmp(argv[1], "identity-verification"))
            return synthetic_mode("verification", 1, true);
        if (!strcmp(argv[1], "identity-heldout"))
            return synthetic_mode("heldout", 1, true);
        if (!strcmp(argv[1], "heldout"))
            return synthetic_mode("heldout", repetitions, false);
    }
    fputs("Usage: l1_diagnostics development|verification|heldout [1..100]\n"
          "       l1_diagnostics identity-development|identity-verification|identity-heldout\n"
          "       l1_diagnostics trace FILE live-default|v06-trained\n", stderr);
    return 2;
}
