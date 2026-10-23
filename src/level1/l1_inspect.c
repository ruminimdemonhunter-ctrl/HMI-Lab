#define _POSIX_C_SOURCE 200809L
#include "l1_demo.h"
#include "l1_shadow.h"
#include "l1_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { INSPECT_MAX_DETAILS = 2000, INSPECT_DEFAULT_DETAILS = 60 };

typedef struct {
    uint64_t time_ms;
    L1Observation observation;
    L1Policy policy;
    L1Action published;
    L1Action fixed;
    L1Action candidate;
    uint16_t candidate_streak;
    uint16_t memory_streak;
    uint8_t static_live_mask;
    uint8_t static_v06_mask;
    uint8_t shadow_mask;
    uint8_t feedback_mask;
    bool shadow_prediction;
    bool feedback_ready;
    bool fallback;
    const char *reason;
    const char *shadow_status;
    const char *event;
    const char *event_status;
} InspectDetail;

typedef struct {
    L1Core core;
    L1Core reference;
    L1Core fixed;
    L1Shadow shadow;
    uint64_t rows;
    uint64_t action_count[4];
    uint64_t fixed_differences;
    uint64_t observer_mismatches;
    uint64_t feedback_windows;
    uint64_t fallbacks;
    uint64_t checksum;
    uint32_t detail_limit;
    uint32_t detail_count;
    InspectDetail details[INSPECT_MAX_DETAILS];
} InspectRun;

static const L1ForecastThresholds trained_thresholds = {2000, 75, 600};

static void usage(FILE *stream) {
    fputs("usage:\n"
          "  ./l1_inspect demo [CASE_NAME] [--details MAX_ROWS]\n"
          "  ./l1_inspect trace ABSOLUTE_GUEST_PATH "
          "[--forecast-config live-default|v06-trained] [--details MAX_ROWS]\n"
          "  ./l1_inspect help\n", stream);
}

static bool decimal_u32(const char *text, uint32_t maximum, uint32_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint32_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static const char *mask_name(uint8_t mask) {
    switch (mask & 7U) {
        case 0: return "none";
        case 1: return "cpu";
        case 2: return "memory";
        case 3: return "cpu+memory";
        case 4: return "io";
        case 5: return "cpu+io";
        case 6: return "memory+io";
        default: return "cpu+memory+io";
    }
}

static const char *condition_name(L1Observation observation,
                                  const L1Policy *policy) {
    if (!observation.valid) return "invalid";
    uint8_t mask = 0;
    if (observation.cpu_some_bp >= policy->cpu_trigger_bp) mask |= 1U;
    if (observation.memory_full_bp >= policy->memory_trigger_bp) mask |= 2U;
    if (observation.io_full_bp >= policy->io_trigger_bp) mask |= 4U;
    return mask_name(mask);
}

static const char *diagnostic_reason(const L1Core *before,
                                     const L1Core *after,
                                     L1Observation observation,
                                     uint64_t time_ms) {
    if (!observation.valid || observation.cpu_some_bp > 10000 ||
        observation.memory_full_bp > 10000 || observation.io_full_bp > 10000)
        return "fallback-invalid-telemetry";
    if (before->monotonic_initialized && time_ms < before->last_monotonic_ms)
        return "fallback-monotonic-regression";
    if (after->action != before->action) return "confirmed-transition";
    if (after->memory_candidate_streak != 0)
        return "memory-confirmation-pending";
    if (after->candidate != after->action && after->candidate_streak != 0)
        return "confirmation-pending";
    return "stable-or-no-confirmed-change";
}

static bool apply_event(L1Core *core, L1DemoEvent event) {
    if (event == L1_DEMO_EVENT_NONE) return true;
    if (event == L1_DEMO_EVENT_APPLY_POLICY) {
        L1Policy policy = l1_demo_valid_policy();
        return l1_apply_policy(core, &policy);
    }
    if (event == L1_DEMO_EVENT_REJECT_POLICY) {
        L1Policy policy = l1_demo_invalid_policy();
        return !l1_apply_policy(core, &policy);
    }
    return l1_rollback_policy(core, 2);
}

static void run_init(InspectRun *run, uint32_t details,
                     L1ForecastThresholds thresholds) {
    memset(run, 0, sizeof(*run));
    l1_init(&run->core);
    l1_init(&run->reference);
    l1_init(&run->fixed);
    (void)l1_set_adaptation_mode(&run->fixed, L1_ADAPT_DISABLED);
    l1_shadow_init(&run->shadow, thresholds);
    run->detail_limit = details;
    run->checksum = UINT64_C(1469598103934665603);
}

static bool run_row(InspectRun *run, const L1DemoRow *row) {
    bool event_main = apply_event(&run->core, row->event);
    bool event_reference = apply_event(&run->reference, row->event);
    bool event_fixed = apply_event(&run->fixed, row->event);
    if (event_main != event_reference || event_main != event_fixed) return false;

    L1Core before = run->core;
    uint32_t reset_before = run->shadow.reset_count;
    L1Action published = l1_step_at(&run->core, row->observation,
                                    row->monotonic_ms);
    L1Action reference = l1_step_at(&run->reference, row->observation,
                                    row->monotonic_ms);
    L1Action fixed = l1_step_at(&run->fixed, row->observation,
                                row->monotonic_ms);
    L1ShadowStep shadow_step = l1_shadow_step_at(
        &run->shadow, row->observation, row->counters, row->monotonic_ms);
    L1Prediction static_live = {0}, static_v06 = {0};
    bool live_valid = l1_fixed_prediction(row->observation,
        l1_default_forecast_thresholds(), &static_live);
    bool v06_valid = l1_fixed_prediction(row->observation,
        trained_thresholds, &static_v06);

    if (published != reference || memcmp(&run->core, &run->reference,
                                         sizeof(run->core)) != 0)
        run->observer_mismatches++;
    if (published != fixed) run->fixed_differences++;
    if ((unsigned)published <= (unsigned)L1_DEFER_IO)
        run->action_count[published]++;

    const char *reason = diagnostic_reason(&before, &run->core,
                                           row->observation,
                                           row->monotonic_ms);
    bool fallback = strncmp(reason, "fallback-", 9) == 0;
    if (fallback) run->fallbacks++;
    if (shadow_step.feedback_ready) run->feedback_windows++;

    const char *shadow_status = "pending";
    if (!row->observation.valid || !row->counters.valid)
        shadow_status = "unavailable-invalid";
    else if (run->shadow.reset_count != reset_before)
        shadow_status = "reset";
    else if (shadow_step.feedback_ready)
        shadow_status = "evaluated";
    else if (shadow_step.prediction_issued)
        shadow_status = "issued";

    L1Prediction shown_prediction = shadow_step.prediction_issued ?
        shadow_step.prediction : run->shadow.pending_prediction;
    char canonical[384];
    int length = snprintf(canonical, sizeof(canonical),
        "R,%" PRIu64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s\n",
        row->monotonic_ms, row->observation.valid ? 1U : 0U,
        row->observation.cpu_some_bp, row->observation.memory_full_bp,
        row->observation.io_full_bp, run->core.policy.generation,
        (unsigned)published, (unsigned)fixed, (unsigned)run->core.candidate,
        run->core.candidate_streak, run->core.memory_candidate_streak,
        shown_prediction.positive_mask,
        shadow_step.feedback_ready ? shadow_step.feedback.positive_mask : 0U,
        reason, l1_demo_event_name(row->event));
    if (length < 0 || (size_t)length >= sizeof(canonical)) return false;
    run->checksum = l1_trace_hash_bytes(run->checksum, canonical,
                                        (size_t)length);

    if (run->detail_count < run->detail_limit) {
        InspectDetail *detail = &run->details[run->detail_count++];
        const char *event_status = event_main ? "accepted" : "failed";
        if (row->event == L1_DEMO_EVENT_NONE)
            event_status = "not-applicable";
        else if (row->event == L1_DEMO_EVENT_REJECT_POLICY && event_main)
            event_status = "rejected-as-expected";
        *detail = (InspectDetail){
            row->monotonic_ms, row->observation, run->core.policy,
            published, fixed, run->core.candidate,
            run->core.candidate_streak, run->core.memory_candidate_streak,
            live_valid ? static_live.positive_mask : 0,
            v06_valid ? static_v06.positive_mask : 0,
            shown_prediction.positive_mask,
            shadow_step.feedback_ready ? shadow_step.feedback.positive_mask : 0,
            shadow_step.prediction_issued || run->shadow.pending,
            shadow_step.feedback_ready, fallback, reason, shadow_status,
            l1_demo_event_name(row->event), event_status
        };
    }
    run->rows++;
    return true;
}

static void print_run(const char *source, const char *name, uint32_t seed,
                      const char *config_name, const InspectRun *run) {
    printf("CASE source=%s name=%s seed=%08x forecast_config=%s rows=%" PRIu64
           " policy_generation=%u policy_cpu=%u policy_memory=%u policy_io=%u"
           " confirmations=%u\n",
           source, name, seed, config_name, run->rows,
           run->core.policy.generation, run->core.policy.cpu_trigger_bp,
           run->core.policy.memory_trigger_bp, run->core.policy.io_trigger_bp,
           run->core.policy.confirmation_samples);
    puts("COLUMNS time_ms valid cpu_bp memory_bp io_bp threshold_hits "
         "published fixed candidate streak memory_streak reason fallback "
         "static_live static_v06 shadow_prediction shadow_status feedback "
         "event event_status generation");
    for (uint32_t i = 0; i < run->detail_count; ++i) {
        const InspectDetail *d = &run->details[i];
        printf("ROW time_ms=%" PRIu64 " valid=%u cpu_bp=%u memory_bp=%u io_bp=%u"
               " threshold_hits=%s published=%s fixed=%s candidate=%s"
               " streak=%u memory_streak=%u reason=%s fallback=%u"
               " static_live=%s static_v06=%s shadow_prediction=%s"
               " shadow_status=%s feedback=%s event=%s event_status=%s"
               " generation=%u\n",
               d->time_ms, d->observation.valid ? 1U : 0U,
               d->observation.cpu_some_bp, d->observation.memory_full_bp,
               d->observation.io_full_bp,
               condition_name(d->observation, &d->policy),
               l1_action_name(d->published), l1_action_name(d->fixed),
               l1_action_name(d->candidate), d->candidate_streak,
               d->memory_streak, d->reason, d->fallback ? 1U : 0U,
               mask_name(d->static_live_mask), mask_name(d->static_v06_mask),
               d->shadow_prediction ? mask_name(d->shadow_mask) : "unavailable",
               d->shadow_status,
               d->feedback_ready ? mask_name(d->feedback_mask) : "unavailable",
               d->event, d->event_status, d->policy.generation);
    }
    printf("SUMMARY name=%s rows=%" PRIu64 " hold=%" PRIu64
           " interactive=%" PRIu64 " protect_memory=%" PRIu64
           " defer_io=%" PRIu64 " fallbacks=%" PRIu64
           " feedback_windows=%" PRIu64 " fixed_differences=%" PRIu64
           " observer_mismatches=%" PRIu64 " checksum=%016" PRIx64 "\n",
           name, run->rows, run->action_count[L1_HOLD],
           run->action_count[L1_PREFER_INTERACTIVE],
           run->action_count[L1_PROTECT_MEMORY],
           run->action_count[L1_DEFER_IO], run->fallbacks,
           run->feedback_windows, run->fixed_differences,
           run->observer_mismatches, run->checksum);
    printf("SAFETY observer_equivalence=%s published_action_source=l1_step_at"
           " comparator_source=l1_step_at shadow_action_coupling=none\n",
           run->observer_mismatches == 0 ? "PASS" : "FAIL");
}

static int run_demo_case(const L1DemoCase *demo, uint32_t details) {
    InspectRun run;
    run_init(&run, details < demo->rows ? details : demo->rows,
             l1_default_forecast_thresholds());
    for (uint32_t i = 0; i < demo->rows; ++i) {
        L1DemoRow row;
        if (!l1_demo_row(demo, i, &row) || !run_row(&run, &row)) {
            fprintf(stderr, "demo execution failed: %s row %u\n", demo->name, i);
            return EXIT_FAILURE;
        }
    }
    print_run("synthetic", demo->name, demo->seed, "live-default", &run);
    return run.observer_mismatches == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

typedef struct { InspectRun *run; } TraceContext;

static bool trace_row_callback(const L1TraceRow *trace_row, void *context) {
    TraceContext *trace = context;
    L1DemoRow row = {trace_row->relative_ms, trace_row->observation,
                     trace_row->counters, L1_DEMO_EVENT_NONE};
    return run_row(trace->run, &row);
}

static bool safe_absolute_path(const char *path) {
    if (path == NULL || path[0] != '/' || strlen(path) > 4095U) return false;
    const char *component = path;
    while ((component = strstr(component, "..")) != NULL) {
        bool left = component == path || component[-1] == '/';
        bool right = component[2] == '\0' || component[2] == '/';
        if (left && right) return false;
        component += 2;
    }
    return true;
}

static bool same_file(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_size == right->st_size &&
        left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
        left->st_mtim.tv_nsec == right->st_mtim.tv_nsec;
}

static int run_trace(const char *path, uint32_t details,
                     L1ForecastThresholds thresholds,
                     const char *config_name) {
    if (!safe_absolute_path(path)) {
        fputs("trace path must be bounded, absolute, and contain no '..' component\n",
              stderr);
        return EXIT_FAILURE;
    }
    struct stat before, after;
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode)) {
        fputs("trace must name an existing regular file\n", stderr);
        return EXIT_FAILURE;
    }
    L1TraceInfo verified, replayed;
    char error[160] = {0};
    if (!l1_trace_read(path, NULL, NULL, &verified, error, sizeof(error))) {
        fprintf(stderr, "trace validation failed: %s\n", error);
        return EXIT_FAILURE;
    }
    InspectRun run;
    run_init(&run, details, thresholds);
    TraceContext context = {&run};
    if (!l1_trace_read(path, trace_row_callback, &context, &replayed,
                       error, sizeof(error)) ||
        stat(path, &after) != 0 || !same_file(&before, &after) ||
        verified.count != replayed.count ||
        verified.checksum != replayed.checksum ||
        verified.bytes != replayed.bytes) {
        fprintf(stderr, "trace replay changed or failed validation: %s\n", error);
        return EXIT_FAILURE;
    }
    puts("TRACE verification=complete schema=V1 two_pass_identity=matched");
    print_run("private-v1-trace", "verified-private-trace", 0,
              config_name, &run);
    printf("TRACE_INFO rows=%" PRIu64 " bytes=%" PRIu64
           " cadence_ms=%u checksum=%016" PRIx64 " horizon_eligible=%u\n",
           verified.count, verified.bytes, verified.cadence_ms,
           verified.checksum, verified.horizon_eligible ? 1U : 0U);
    return run.observer_mismatches == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if (argc < 2) { usage(stderr); return EXIT_FAILURE; }
    puts("HMI_L1_INSPECT_V1 mode=read-only action_authority=advisory");

    if (strcmp(argv[1], "demo") == 0) {
        const char *name = NULL;
        uint32_t details = INSPECT_DEFAULT_DETAILS;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--details") == 0 && i + 1 < argc) {
                if (!decimal_u32(argv[++i], INSPECT_MAX_DETAILS, &details)) {
                    fputs("invalid --details value\n", stderr);
                    return EXIT_FAILURE;
                }
            } else if (argv[i][0] != '-' && name == NULL) {
                name = argv[i];
            } else {
                fputs("invalid demo argument\n", stderr);
                return EXIT_FAILURE;
            }
        }
        if (name != NULL) {
            const L1DemoCase *demo = l1_demo_case_named(name);
            if (demo == NULL) {
                fputs("unknown demo case\n", stderr);
                return EXIT_FAILURE;
            }
            return run_demo_case(demo, details);
        }
        for (size_t i = 0; i < l1_demo_case_count(); ++i)
            if (run_demo_case(l1_demo_case_at(i), details) != EXIT_SUCCESS)
                return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "trace") == 0) {
        if (argc < 3) { usage(stderr); return EXIT_FAILURE; }
        uint32_t details = 0;
        L1ForecastThresholds thresholds = l1_default_forecast_thresholds();
        const char *config = "live-default";
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--details") == 0 && i + 1 < argc) {
                if (!decimal_u32(argv[++i], INSPECT_MAX_DETAILS, &details)) {
                    fputs("invalid --details value\n", stderr);
                    return EXIT_FAILURE;
                }
            } else if (strcmp(argv[i], "--forecast-config") == 0 &&
                       i + 1 < argc) {
                config = argv[++i];
                if (strcmp(config, "live-default") == 0) {
                    thresholds = l1_default_forecast_thresholds();
                } else if (strcmp(config, "v06-trained") == 0) {
                    thresholds = trained_thresholds;
                } else {
                    fputs("invalid --forecast-config value\n", stderr);
                    return EXIT_FAILURE;
                }
            } else {
                fputs("invalid trace argument\n", stderr);
                return EXIT_FAILURE;
            }
        }
        return run_trace(argv[2], details, thresholds, config);
    }
    usage(stderr);
    return EXIT_FAILURE;
}
