#define _POSIX_C_SOURCE 200809L

#include "../l1.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

enum {
    FAMILY_COUNT = 10,
    CONTROLLER_COUNT = 3,
    MAX_SAMPLES = 1800,
    MAX_EVENTS = 256,
    MAX_SCENARIOS = 50
};

static const uint64_t development_seeds[] = {41, 43};
static const uint64_t training_seeds[] = {101, 211, 307, 401};
static const uint64_t heldout_seeds[] = {1009, 2003, 3001, 4001, 5003};
static const char *family_names[FAMILY_COUNT] = {
    "idle", "short-false-spikes", "sustained-cpu", "sustained-memory",
    "sustained-io", "overlapping-events", "threshold-oscillation",
    "changing-regime", "invalid-data", "irregular-regressing-time"
};
static const size_t family_lengths[FAMILY_COUNT] = {
    300, 450, 900, 900, 900, 1200, 1800, 1800, 1200, 1200
};
static const char *controller_names[CONTROLLER_COUNT] = {
    "frozen-default", "tuned-fixed", "bounded-adaptive"
};
static const uint64_t frozen_selected_policy_fingerprint =
    UINT64_C(0x5147f61840da7089);

typedef struct {
    uint64_t state;
    uint64_t increment;
} Pcg32;

typedef struct {
    L1Observation observation;
    L1Action truth;
    uint64_t timestamp_ms;
    bool regressing_time;
} EvalSample;

typedef struct {
    uint64_t seed;
    unsigned family;
    size_t count;
    EvalSample samples[MAX_SAMPLES];
} EvalTrace;

typedef struct {
    uint64_t samples;
    uint64_t false_positives;
    uint64_t missed[4];
    uint64_t events[4];
    uint64_t undetected[4];
    uint64_t switches;
    uint64_t adaptations;
    uint64_t invalid;
    uint64_t regressions;
    uint64_t fallback_violations;
    uint8_t debounce_min;
    uint8_t debounce_max;
    uint32_t delays[4][MAX_EVENTS];
    size_t delay_count[4];
    uint64_t scenario_loss_q[MAX_SCENARIOS];
    size_t scenario_count;
    uint64_t loss_q;
} Metrics;

typedef struct {
    Metrics by_family[FAMILY_COUNT];
    Metrics aggregate;
} ControllerMetrics;

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint32_t pcg32_next(Pcg32 *rng) {
    uint64_t old = rng->state;
    uint32_t xorshifted;
    uint32_t rotation;
    rng->state = old * UINT64_C(6364136223846793005) + rng->increment;
    xorshifted = (uint32_t)(((old >> 18) ^ old) >> 27);
    rotation = (uint32_t)(old >> 59);
    return (xorshifted >> rotation) | (xorshifted << ((-rotation) & 31));
}

static void pcg32_seed(Pcg32 *rng, uint64_t state, uint64_t stream) {
    rng->state = 0;
    rng->increment = (stream << 1) | 1;
    (void)pcg32_next(rng);
    rng->state += state;
    (void)pcg32_next(rng);
}

static int uniform_int(Pcg32 *rng, int low, int high) {
    uint32_t bound = (uint32_t)(high - low + 1);
    uint32_t threshold = (uint32_t)(-bound) % bound;
    uint32_t value;
    do value = pcg32_next(rng); while (value < threshold);
    return low + (int)(value % bound);
}

static uint16_t clamp_bp(int value) {
    if (value < 0) return 0;
    if (value > 10000) return 10000;
    return (uint16_t)value;
}

static bool in_interval(size_t i, int start, int duration) {
    return (int)i >= start && (int)i < start + duration;
}

static L1Action oracle_action(bool cpu, bool memory, bool io) {
    if (memory) return L1_PROTECT_MEMORY;
    if (io) return L1_DEFER_IO;
    if (cpu) return L1_PREFER_INTERACTIVE;
    return L1_HOLD;
}

static uint64_t fnv_add_u64(uint64_t hash, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) {
        hash ^= (uint8_t)(value >> (i * 8));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t policy_fingerprint(const L1Policy *policy) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = fnv_add_u64(hash, policy->schema_version, 4);
    hash = fnv_add_u64(hash, policy->generation, 4);
    hash = fnv_add_u64(hash, policy->cpu_trigger_bp, 2);
    hash = fnv_add_u64(hash, policy->memory_trigger_bp, 2);
    hash = fnv_add_u64(hash, policy->io_trigger_bp, 2);
    return fnv_add_u64(hash, policy->confirmation_samples, 1);
}

static bool is_frozen_selected_policy(const L1Policy *policy) {
    return policy->schema_version == 1 && policy->generation == 1 &&
        policy->cpu_trigger_bp == 3000 && policy->memory_trigger_bp == 100 &&
        policy->io_trigger_bp == 500 && policy->confirmation_samples == 2 &&
        policy_fingerprint(policy) == frozen_selected_policy_fingerprint;
}

static uint64_t checksum_trace(uint64_t hash, const EvalTrace *trace) {
    hash = fnv_add_u64(hash, trace->seed, 8);
    hash = fnv_add_u64(hash, trace->family, 4);
    hash = fnv_add_u64(hash, trace->count, 8);
    for (size_t i = 0; i < trace->count; i++) {
        const EvalSample *s = &trace->samples[i];
        hash = fnv_add_u64(hash, s->observation.cpu_some_bp, 2);
        hash = fnv_add_u64(hash, s->observation.memory_full_bp, 2);
        hash = fnv_add_u64(hash, s->observation.io_full_bp, 2);
        hash = fnv_add_u64(hash, s->observation.valid ? 1 : 0, 1);
        hash = fnv_add_u64(hash, (uint64_t)s->truth, 1);
        hash = fnv_add_u64(hash, s->timestamp_ms, 8);
        hash = fnv_add_u64(hash, s->regressing_time ? 1 : 0, 1);
    }
    return hash;
}

static void generate_trace(EvalTrace *trace, uint64_t seed, unsigned family) {
    uint64_t mixer = seed ^ (UINT64_C(0xd1b54a32d192ed03) * (family + 1));
    uint64_t truth_state = splitmix64(&mixer);
    uint64_t truth_stream = splitmix64(&mixer);
    uint64_t noise_state = splitmix64(&mixer);
    uint64_t noise_stream = splitmix64(&mixer);
    Pcg32 truth_rng;
    Pcg32 noise_rng;
    int starts[4] = {0};
    int durations[4] = {0};
    int spike_starts[8] = {0};
    int spike_lengths[8] = {0};
    int spike_channels[8] = {0};
    uint64_t timestamp = 0;
    uint64_t accepted_max = 0;

    pcg32_seed(&truth_rng, truth_state, truth_stream);
    pcg32_seed(&noise_rng, noise_state, noise_stream);
    trace->seed = seed;
    trace->family = family;
    trace->count = family_lengths[family];

    for (unsigned e = 0; e < 4; e++) {
        starts[e] = 100 + (int)e * 240 + uniform_int(&truth_rng, -20, 20);
        durations[e] = 105 + uniform_int(&truth_rng, -25, 35);
    }
    for (unsigned e = 0; e < 8; e++) {
        spike_starts[e] = 20 + (int)e * 50 + uniform_int(&truth_rng, -8, 8);
        spike_lengths[e] = uniform_int(&truth_rng, 1, 4);
        spike_channels[e] = uniform_int(&truth_rng, 0, 2);
    }

    for (size_t i = 0; i < trace->count; i++) {
        bool cpu = false, memory = false, io = false;
        bool valid = true;
        int cpu_bp = 600, memory_bp = 10, io_bp = 50;
        uint64_t increment = 1000;

        if (family == 2 || family == 3 || family == 4) {
            bool active = false;
            for (unsigned e = 0; e < 3; e++)
                active = active || in_interval(i, starts[e], durations[e]);
            cpu = family == 2 && active;
            memory = family == 3 && active;
            io = family == 4 && active;
        } else if (family == 5) {
            cpu = in_interval(i, 140, 360) || in_interval(i, 840, 230);
            io = in_interval(i, 300, 390);
            memory = in_interval(i, 500, 270);
        } else if (family == 6) {
            cpu = in_interval(i, 1400, 100);
            memory = in_interval(i, 1600, 100);
        } else if (family == 7) {
            io = in_interval(i, 650, 110);
            cpu = in_interval(i, 1150, 130);
            memory = in_interval(i, 1500, 150);
        } else if (family == 8) {
            cpu = in_interval(i, 250, 180);
            memory = in_interval(i, 700, 200);
            valid = !(in_interval(i, 80, 18) || in_interval(i, 280, 25) ||
                      in_interval(i, 745, 30) || in_interval(i, 1050, 20));
        } else if (family == 9) {
            io = in_interval(i, 250, 200);
            cpu = in_interval(i, 700, 150);
        }

        if (cpu) cpu_bp = 3600;
        if (memory) memory_bp = 180;
        if (io) io_bp = 750;

        if (family == 1) {
            for (unsigned e = 0; e < 8; e++) {
                if (in_interval(i, spike_starts[e], spike_lengths[e])) {
                    if (spike_channels[e] == 0) cpu_bp = 5200;
                    if (spike_channels[e] == 1) memory_bp = 350;
                    if (spike_channels[e] == 2) io_bp = 1200;
                }
            }
        } else if (family == 6 && i < 1300) {
            unsigned phase = (unsigned)(i % 80);
            if (phase < 12) cpu_bp = 3200;
            else if (phase < 24) cpu_bp = 2700;
            if (phase >= 35 && phase < 47) io_bp = 580;
            else if (phase >= 47 && phase < 59) io_bp = 360;
        } else if (family == 7) {
            if (i < 600) {
                cpu_bp += 700;
            } else if (i < 1200) {
                io_bp += 180;
                if (!io && i % 90 < 8) io_bp = 650;
            } else {
                cpu_bp += 1100;
                memory_bp += 18;
                if (!memory && i % 110 < 9) memory_bp = 125;
            }
        }

        cpu_bp += uniform_int(&noise_rng, -400, 400);
        memory_bp += uniform_int(&noise_rng, -40, 40);
        io_bp += uniform_int(&noise_rng, -150, 150);

        if (family == 9) {
            static const uint64_t increments[] = {250, 500, 1000, 1750, 2500};
            increment = increments[pcg32_next(&noise_rng) % 5];
        }
        timestamp += increment;
        if (family == 9 && (i == 350 || i == 800)) timestamp -= 5000;

        trace->samples[i].observation = (L1Observation) {
            .cpu_some_bp = clamp_bp(cpu_bp),
            .memory_full_bp = clamp_bp(memory_bp),
            .io_full_bp = clamp_bp(io_bp),
            .valid = valid
        };
        trace->samples[i].truth = oracle_action(cpu, memory, io);
        trace->samples[i].timestamp_ms = timestamp;
        trace->samples[i].regressing_time = i > 0 && timestamp < accepted_max;
        if (timestamp >= accepted_max) accepted_max = timestamp;
    }
}

static void metrics_init(Metrics *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    metrics->debounce_min = UINT8_MAX;
}

static void controller_metrics_init(ControllerMetrics *metrics) {
    metrics_init(&metrics->aggregate);
    for (unsigned family = 0; family < FAMILY_COUNT; family++)
        metrics_init(&metrics->by_family[family]);
}

static uint64_t scenario_loss_q(const Metrics *m) {
    return m->false_positives * 4 + m->missed[L1_PREFER_INTERACTIVE] * 16 +
           m->missed[L1_DEFER_IO] * 24 + m->missed[L1_PROTECT_MEMORY] * 40 +
           m->switches;
}

static void add_metrics(Metrics *destination, const Metrics *source) {
    destination->samples += source->samples;
    destination->false_positives += source->false_positives;
    destination->switches += source->switches;
    destination->adaptations += source->adaptations;
    destination->invalid += source->invalid;
    destination->regressions += source->regressions;
    destination->fallback_violations += source->fallback_violations;
    if (source->debounce_min < destination->debounce_min)
        destination->debounce_min = source->debounce_min;
    if (source->debounce_max > destination->debounce_max)
        destination->debounce_max = source->debounce_max;
    for (unsigned action = 1; action <= 3; action++) {
        destination->missed[action] += source->missed[action];
        destination->events[action] += source->events[action];
        destination->undetected[action] += source->undetected[action];
        for (size_t i = 0; i < source->delay_count[action]; i++) {
            if (destination->delay_count[action] >= MAX_EVENTS) {
                fprintf(stderr, "too many detection-delay samples\n");
                exit(2);
            }
            destination->delays[action][destination->delay_count[action]++] =
                source->delays[action][i];
        }
    }
    destination->loss_q += source->loss_q;
    if (destination->scenario_count >= MAX_SCENARIOS) {
        fprintf(stderr, "too many scenarios\n");
        exit(2);
    }
    destination->scenario_loss_q[destination->scenario_count++] = source->loss_q;
}

static Metrics run_trace(const EvalTrace *trace, const L1Policy *policy,
                         bool adaptive) {
    Metrics result;
    L1Core core;
    L1Action previous = L1_HOLD;
    L1Action event_truth = L1_HOLD;
    size_t event_start = 0;
    bool event_detected = false;
    metrics_init(&result);
    l1_init(&core);
    if (policy != NULL && !l1_apply_policy(&core, policy)) {
        fprintf(stderr, "internal error: selected policy rejected\n");
        exit(2);
    }
    l1_set_adaptation_enabled(&core, adaptive);
    result.debounce_min = core.debounce_extra;

    for (size_t i = 0; i < trace->count; i++) {
        const EvalSample *sample = &trace->samples[i];
        uint8_t old_extra = core.debounce_extra;
        L1Action actual;

        if (sample->truth != event_truth) {
            if (event_truth != L1_HOLD && !event_detected)
                result.undetected[event_truth]++;
            event_truth = sample->truth;
            event_detected = event_truth == L1_HOLD;
            event_start = i;
            if (event_truth != L1_HOLD) result.events[event_truth]++;
        }

        actual = l1_step_at(&core, sample->observation, sample->timestamp_ms);
        result.samples++;
        if (!sample->observation.valid) result.invalid++;
        if (sample->regressing_time) result.regressions++;
        if ((!sample->observation.valid || sample->regressing_time) &&
            actual != L1_HOLD)
            result.fallback_violations++;
        if (sample->truth == L1_HOLD) {
            if (actual != L1_HOLD) result.false_positives++;
        } else {
            if (actual != sample->truth) result.missed[sample->truth]++;
            if (!event_detected && actual == sample->truth) {
                size_t count = result.delay_count[sample->truth];
                if (count >= MAX_EVENTS) {
                    fprintf(stderr, "too many events in scenario\n");
                    exit(2);
                }
                result.delays[sample->truth][count] = (uint32_t)(i - event_start);
                result.delay_count[sample->truth]++;
                event_detected = true;
            }
        }
        if (i > 0 && actual != previous) result.switches++;
        previous = actual;
        if (core.debounce_extra != old_extra) result.adaptations++;
        if (core.debounce_extra < result.debounce_min)
            result.debounce_min = core.debounce_extra;
        if (core.debounce_extra > result.debounce_max)
            result.debounce_max = core.debounce_extra;
        if (core.debounce_extra > L1_MAX_EXTRA_CONFIRMATIONS) {
            fprintf(stderr, "adaptation escaped compiled bound\n");
            exit(2);
        }
    }
    if (event_truth != L1_HOLD && !event_detected)
        result.undetected[event_truth]++;
    result.loss_q = scenario_loss_q(&result);
    return result;
}

static EvalTrace *generate_split(const uint64_t *seeds, size_t seed_count,
                                 uint64_t *checksum) {
    size_t count = seed_count * FAMILY_COUNT;
    EvalTrace *traces = calloc(count, sizeof(*traces));
    uint64_t hash = UINT64_C(1469598103934665603);
    if (traces == NULL) return NULL;
    for (size_t seed_i = 0; seed_i < seed_count; seed_i++) {
        for (unsigned family = 0; family < FAMILY_COUNT; family++) {
            EvalTrace *trace = &traces[seed_i * FAMILY_COUNT + family];
            generate_trace(trace, seeds[seed_i], family);
            hash = checksum_trace(hash, trace);
        }
    }
    *checksum = hash;
    return traces;
}

static void evaluate_once(const EvalTrace *traces, size_t count,
                          const L1Policy *tuned, ControllerMetrics output[3],
                          unsigned order_offset, uint64_t elapsed_ns[3]) {
    for (unsigned order = 0; order < CONTROLLER_COUNT; order++) {
        unsigned controller = (order + order_offset) % CONTROLLER_COUNT;
        struct timespec before, after;
        (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before);
        for (size_t i = 0; i < count; i++) {
            const L1Policy *policy = controller == 0 ? NULL : tuned;
            bool adaptive = controller == 2;
            Metrics scenario = run_trace(&traces[i], policy, adaptive);
            add_metrics(&output[controller].by_family[traces[i].family], &scenario);
            add_metrics(&output[controller].aggregate, &scenario);
        }
        (void)clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &after);
        elapsed_ns[controller] +=
            (uint64_t)(after.tv_sec - before.tv_sec) * UINT64_C(1000000000) +
            (uint64_t)(after.tv_nsec - before.tv_nsec);
    }
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int compare_u32(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static void distribution_u64(const uint64_t *values, size_t count,
                             uint64_t *minimum, uint64_t *median,
                             uint64_t *maximum) {
    uint64_t copy[MAX_SCENARIOS];
    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_u64);
    *minimum = copy[0];
    *median = copy[(count - 1) / 2];
    *maximum = copy[count - 1];
}

static void delay_summary(const Metrics *metrics, L1Action action,
                          double *mean, uint32_t *p95, uint32_t *maximum) {
    size_t count = metrics->delay_count[action];
    uint32_t copy[MAX_EVENTS];
    uint64_t sum = 0;
    if (count == 0) {
        *mean = 0.0;
        *p95 = *maximum = 0;
        return;
    }
    memcpy(copy, metrics->delays[action], count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_u32);
    for (size_t i = 0; i < count; i++) sum += copy[i];
    *mean = (double)sum / (double)count;
    *p95 = copy[((95 * count + 99) / 100) - 1];
    *maximum = copy[count - 1];
}

static void print_metrics_row(const char *split, const char *controller,
                              const char *family, const Metrics *metrics,
                              double cpu_ns, long rss_kib) {
    uint64_t loss_min, loss_median, loss_max;
    double cpu_mean, io_mean, memory_mean;
    uint32_t cpu_p95, cpu_max, io_p95, io_max, memory_p95, memory_max;
    distribution_u64(metrics->scenario_loss_q, metrics->scenario_count,
                     &loss_min, &loss_median, &loss_max);
    delay_summary(metrics, L1_PREFER_INTERACTIVE, &cpu_mean, &cpu_p95, &cpu_max);
    delay_summary(metrics, L1_DEFER_IO, &io_mean, &io_p95, &io_max);
    delay_summary(metrics, L1_PROTECT_MEMORY,
                  &memory_mean, &memory_p95, &memory_max);
    printf("RESULT,%s,%s,%s,%zu,%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,"
           "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
           "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
           "%" PRIu64 ",%" PRIu64 ",%.3f,%u,%u,%.3f,%u,%u,%.3f,%u,%u,"
           "%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRIu64 ",%" PRIu64 ","
           "%" PRIu64 ",%.3f,%ld\n",
           split, controller, family, metrics->scenario_count, metrics->samples,
           metrics->loss_q / 4.0, loss_min / 4.0, loss_median / 4.0,
           loss_max / 4.0, metrics->false_positives,
           metrics->missed[L1_PREFER_INTERACTIVE], metrics->missed[L1_DEFER_IO],
           metrics->missed[L1_PROTECT_MEMORY],
           metrics->events[L1_PREFER_INTERACTIVE],
           metrics->events[L1_DEFER_IO], metrics->events[L1_PROTECT_MEMORY],
           metrics->undetected[L1_PREFER_INTERACTIVE],
           metrics->undetected[L1_DEFER_IO],
           metrics->undetected[L1_PROTECT_MEMORY],
           cpu_mean, cpu_p95, cpu_max, io_mean, io_p95, io_max,
           memory_mean, memory_p95, memory_max, metrics->switches,
           metrics->adaptations,
           metrics->debounce_min == UINT8_MAX ? 0 : metrics->debounce_min,
           metrics->debounce_max, metrics->invalid, metrics->regressions,
           metrics->fallback_violations, cpu_ns, rss_kib);
}

static void print_header(void) {
    puts("HEADER,split,controller,family,scenarios,samples,loss,scenario_loss_min,"
         "scenario_loss_median,scenario_loss_max,false_positives,missed_cpu,"
         "missed_io,missed_memory,events_cpu,events_io,events_memory,"
         "undetected_cpu,undetected_io,undetected_memory,delay_cpu_mean,"
         "delay_cpu_p95,delay_cpu_max,delay_io_mean,delay_io_p95,delay_io_max,"
         "delay_memory_mean,delay_memory_p95,delay_memory_max,switches,"
         "adaptations,debounce_min,debounce_max,invalid,regressions,"
         "fallback_violations,cpu_ns_per_decision,peak_rss_kib");
}

static bool policy_better(const Metrics *candidate, const L1Policy *candidate_p,
                          const Metrics *best, const L1Policy *best_p) {
    if (candidate->loss_q != best->loss_q) return candidate->loss_q < best->loss_q;
#define LOWER_FIELD(field) if (candidate->field != best->field) return candidate->field < best->field
    LOWER_FIELD(missed[L1_PROTECT_MEMORY]);
    LOWER_FIELD(undetected[L1_PROTECT_MEMORY]);
    LOWER_FIELD(missed[L1_DEFER_IO]);
    LOWER_FIELD(undetected[L1_DEFER_IO]);
    LOWER_FIELD(missed[L1_PREFER_INTERACTIVE]);
    LOWER_FIELD(undetected[L1_PREFER_INTERACTIVE]);
    LOWER_FIELD(false_positives);
    LOWER_FIELD(switches);
#undef LOWER_FIELD
    if (candidate_p->confirmation_samples != best_p->confirmation_samples)
        return candidate_p->confirmation_samples < best_p->confirmation_samples;
    if (candidate_p->cpu_trigger_bp != best_p->cpu_trigger_bp)
        return candidate_p->cpu_trigger_bp < best_p->cpu_trigger_bp;
    if (candidate_p->memory_trigger_bp != best_p->memory_trigger_bp)
        return candidate_p->memory_trigger_bp < best_p->memory_trigger_bp;
    return candidate_p->io_trigger_bp < best_p->io_trigger_bp;
}

static bool write_policy(const char *path, const L1Policy *policy) {
    FILE *file = fopen(path, "w");
    if (file == NULL) return false;
    int ok = fprintf(file, "schema_version=%u\ngeneration=%u\n"
                    "cpu_trigger_bp=%u\nmemory_trigger_bp=%u\n"
                    "io_trigger_bp=%u\nconfirmation_samples=%u\n",
                    policy->schema_version, policy->generation,
                    policy->cpu_trigger_bp, policy->memory_trigger_bp,
                    policy->io_trigger_bp, policy->confirmation_samples) > 0;
    if (fclose(file) != 0) ok = 0;
    return ok != 0;
}

static int train(const char *output_path) {
    static const uint16_t cpu_values[] = {2000, 3000, 4000};
    static const uint16_t memory_values[] = {50, 100, 200};
    static const uint16_t io_values[] = {300, 500, 800};
    static const uint8_t confirmation_values[] = {2, 3, 4, 5};
    uint64_t checksum;
    size_t trace_count = sizeof(training_seeds) / sizeof(training_seeds[0]) *
                         FAMILY_COUNT;
    EvalTrace *traces = generate_split(training_seeds,
        sizeof(training_seeds) / sizeof(training_seeds[0]), &checksum);
    Metrics best;
    L1Policy best_policy = {0};
    bool have_best = false;
    unsigned candidates = 0;
    if (traces == NULL) {
        fprintf(stderr, "cannot allocate training traces\n");
        return 2;
    }
    metrics_init(&best);
    for (size_t c = 0; c < sizeof(cpu_values) / sizeof(cpu_values[0]); c++)
    for (size_t m = 0; m < sizeof(memory_values) / sizeof(memory_values[0]); m++)
    for (size_t io = 0; io < sizeof(io_values) / sizeof(io_values[0]); io++)
    for (size_t k = 0; k < sizeof(confirmation_values) / sizeof(confirmation_values[0]); k++) {
        L1Policy policy = {1, 1, cpu_values[c], memory_values[m], io_values[io],
                           confirmation_values[k]};
        Metrics aggregate;
        metrics_init(&aggregate);
        candidates++;
        if (!l1_policy_valid(&policy)) {
            fprintf(stderr, "search produced invalid policy\n");
            free(traces);
            return 2;
        }
        for (size_t i = 0; i < trace_count; i++) {
            Metrics scenario = run_trace(&traces[i], &policy, false);
            add_metrics(&aggregate, &scenario);
        }
        if (!have_best || policy_better(&aggregate, &policy, &best, &best_policy)) {
            best = aggregate;
            best_policy = policy;
            have_best = true;
        }
    }
    if (!write_policy(output_path, &best_policy)) {
        fprintf(stderr, "cannot write selected policy '%s': %s\n",
                output_path, strerror(errno));
        free(traces);
        return 2;
    }
    if (!is_frozen_selected_policy(&best_policy)) {
        fprintf(stderr, "training selection differs from frozen policy identity\n");
        free(traces);
        return 2;
    }
    printf("TRAIN_SELECTED,candidates=%u,seeds=101|211|307|401,checksum=%016" PRIx64
           ",loss=%.2f,cpu=%u,memory=%u,io=%u,confirmation=%u,"
           "policy_fingerprint=%016" PRIx64 ",output=%s\n",
           candidates, checksum, best.loss_q / 4.0, best_policy.cpu_trigger_bp,
           best_policy.memory_trigger_bp, best_policy.io_trigger_bp,
           best_policy.confirmation_samples, policy_fingerprint(&best_policy),
           output_path);
    free(traces);
    return 0;
}

static bool recall_at_least(const Metrics *left, const Metrics *right,
                            L1Action action) {
    uint64_t left_detected = left->events[action] - left->undetected[action];
    uint64_t right_detected = right->events[action] - right->undetected[action];
    if (left->events[action] == 0 || right->events[action] == 0) return true;
    return left_detected * right->events[action] >=
           right_detected * left->events[action];
}

static uint32_t maximum_delay(const Metrics *metrics, L1Action action) {
    uint32_t maximum = 0;
    for (size_t i = 0; i < metrics->delay_count[action]; i++)
        if (metrics->delays[action][i] > maximum) maximum = metrics->delays[action][i];
    return maximum;
}

static int heldout(const char *policy_path, unsigned repetitions) {
    char error[160] = {0};
    L1Policy tuned;
    uint64_t checksum;
    uint64_t elapsed_ns[CONTROLLER_COUNT] = {0};
    size_t seed_count = sizeof(heldout_seeds) / sizeof(heldout_seeds[0]);
    size_t trace_count = seed_count * FAMILY_COUNT;
    EvalTrace *traces;
    ControllerMetrics first[CONTROLLER_COUNT];
    struct rusage usage;
    struct utsname system;

    if (!l1_read_policy(policy_path, &tuned, error, sizeof(error))) {
        fprintf(stderr, "invalid selected policy: %s\n", error);
        return 2;
    }
    if (!is_frozen_selected_policy(&tuned)) {
        fprintf(stderr,
                "selected policy identity mismatch: expected %016" PRIx64
                ", got %016" PRIx64 "\n",
                frozen_selected_policy_fingerprint, policy_fingerprint(&tuned));
        return 2;
    }
    traces = generate_split(heldout_seeds, seed_count, &checksum);
    if (traces == NULL) {
        fprintf(stderr, "cannot allocate held-out traces\n");
        return 2;
    }
    for (unsigned c = 0; c < CONTROLLER_COUNT; c++) controller_metrics_init(&first[c]);
    for (unsigned repetition = 0; repetition < repetitions; repetition++) {
        ControllerMetrics current[CONTROLLER_COUNT];
        for (unsigned c = 0; c < CONTROLLER_COUNT; c++) controller_metrics_init(&current[c]);
        evaluate_once(traces, trace_count, &tuned, current,
                      repetition % CONTROLLER_COUNT, elapsed_ns);
        if (repetition == 0)
            memcpy(first, current, sizeof(first));
    }
    (void)getrusage(RUSAGE_SELF, &usage);
    (void)uname(&system);
    printf("RUN,split=heldout,seeds=1009|2003|3001|4001|5003,checksum=%016" PRIx64
           ",repetitions=%u,policy=%u|%u|%u|%u,policy_fingerprint=%016" PRIx64 "\n",
           checksum, repetitions, tuned.cpu_trigger_bp, tuned.memory_trigger_bp,
           tuned.io_trigger_bp, tuned.confirmation_samples,
           policy_fingerprint(&tuned));
    printf("ENV,compiler=%s,system=%s,machine=%s,sizeof_l1core=%zu,peak_rss_kib=%ld\n",
           __VERSION__, system.sysname, system.machine, sizeof(L1Core), usage.ru_maxrss);
    print_header();
    for (unsigned controller = 0; controller < CONTROLLER_COUNT; controller++) {
        double ns = (double)elapsed_ns[controller] /
                    (double)(first[controller].aggregate.samples * repetitions);
        for (unsigned family = 0; family < FAMILY_COUNT; family++)
            print_metrics_row("heldout", controller_names[controller],
                              family_names[family],
                              &first[controller].by_family[family], ns,
                              usage.ru_maxrss);
        print_metrics_row("heldout", controller_names[controller], "ALL",
                          &first[controller].aggregate, ns, usage.ru_maxrss);
    }

    const Metrics *frozen = &first[0].aggregate;
    const Metrics *fixed = &first[1].aggregate;
    const Metrics *adaptive = &first[2].aggregate;
    bool memory_guard = recall_at_least(adaptive, frozen, L1_PROTECT_MEMORY) &&
        recall_at_least(adaptive, fixed, L1_PROTECT_MEMORY) &&
        adaptive->undetected[L1_PROTECT_MEMORY] <= frozen->undetected[L1_PROTECT_MEMORY] &&
        adaptive->undetected[L1_PROTECT_MEMORY] <= fixed->undetected[L1_PROTECT_MEMORY] &&
        maximum_delay(adaptive, L1_PROTECT_MEMORY) <=
            maximum_delay(fixed, L1_PROTECT_MEMORY);
    bool io_unfavorable = !recall_at_least(adaptive, fixed, L1_DEFER_IO) ||
        adaptive->undetected[L1_DEFER_IO] > fixed->undetected[L1_DEFER_IO] ||
        maximum_delay(adaptive, L1_DEFER_IO) > maximum_delay(fixed, L1_DEFER_IO);
    bool fixed_wins = fixed->loss_q < adaptive->loss_q;
    uint64_t violations = frozen->fallback_violations + fixed->fallback_violations +
                          adaptive->fallback_violations;
    printf("GUARDRAIL,memory=%s,io=%s,fixed_beats_adaptive=%s,fallback_violations=%" PRIu64 "\n",
           memory_guard ? "PASS" : "REGRESSION",
           io_unfavorable ? "UNFAVORABLE" : "PASS",
           fixed_wins ? "YES" : "NO", violations);
    printf("GO_NO_GO,%s\n", (!memory_guard || fixed_wins) ? "NO_GO" : "SIMULATION_ONLY_PASS");
    free(traces);
    return violations == 0 ? 0 : 2;
}

static int development(void) {
    uint64_t checksum;
    size_t seed_count = sizeof(development_seeds) / sizeof(development_seeds[0]);
    EvalTrace *traces = generate_split(development_seeds, seed_count, &checksum);
    uint64_t invalid = 0, regressions = 0;
    if (traces == NULL) return 2;
    for (size_t i = 0; i < seed_count * FAMILY_COUNT; i++) {
        for (size_t s = 0; s < traces[i].count; s++) {
            if (!traces[i].samples[s].observation.valid) invalid++;
            if (traces[i].samples[s].regressing_time) regressions++;
        }
    }
    printf("DEVELOPMENT,scenarios=%zu,checksum=%016" PRIx64
           ",invalid=%" PRIu64 ",regressions=%" PRIu64 "\n",
           seed_count * FAMILY_COUNT, checksum, invalid, regressions);
    free(traces);
    return invalid > 0 && regressions > 0 ? 0 : 2;
}

static bool parse_repetitions(const char *text, unsigned *out) {
    char *end;
    unsigned long value;
    if (text == NULL || *text == '\0' || *text == '-') return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end != '\0' || value < 1 || value > 10000) return false;
    *out = (unsigned)value;
    return true;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s development | train POLICY | heldout POLICY REPETITIONS\n",
            program);
}

int main(int argc, char **argv) {
    unsigned repetitions;
    if (argc == 2 && strcmp(argv[1], "development") == 0) return development();
    if (argc == 3 && strcmp(argv[1], "train") == 0) return train(argv[2]);
    if (argc == 4 && strcmp(argv[1], "heldout") == 0 &&
        parse_repetitions(argv[3], &repetitions))
        return heldout(argv[2], repetitions);
    usage(argv[0]);
    return 2;
}
