#define _POSIX_C_SOURCE 200809L
#include "l1.h"
#include "l1_shadow.h"
#include "l1_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { CONFIG_COUNT = 2, CLASS_COUNT = 3 };

static const char *config_names[CONFIG_COUNT] = {
    "live-default", "v06-trained"
};
static const char *class_names[CLASS_COUNT] = {"cpu", "memory", "io"};
static const L1ForecastThresholds configs[CONFIG_COUNT] = {
    {800, 40, 150}, {2000, 75, 600}
};

typedef struct {
    uint64_t tp, tn, fp, fn;
    uint64_t adaptive_tp, adaptive_tn, adaptive_fp, adaptive_fn;
    uint64_t changed, improved, worsened;
    uint64_t offset_windows, updates;
    uint64_t static_miss_delay_sum_ms, static_miss_delay_max_ms;
    uint64_t adaptive_miss_delay_sum_ms, adaptive_miss_delay_max_ms;
} ClassMetrics;

typedef struct {
    L1Shadow shadow;
    L1Core published;
    L1Core reference;
    uint64_t observations;
    uint64_t issued;
    uint64_t scored;
    uint64_t discarded_invalid;
    uint64_t discarded_gap_late;
    uint64_t discarded_reset;
    uint64_t shadow_resets;
    uint64_t unfinished;
    uint64_t delayed_feedback;
    uint64_t feedback_elapsed_sum_ms;
    uint64_t feedback_elapsed_max_ms;
    uint64_t action_mismatches;
    uint64_t fallback_violations;
    uint64_t first_ms;
    uint64_t last_ms;
    bool have_time;
    bool pending;
    uint8_t pending_static;
    uint8_t pending_adaptive;
    uint8_t pending_changed;
    uint64_t pending_started_ms;
    ClassMetrics classes[CLASS_COUNT];
} EvidenceRun;

typedef struct {
    EvidenceRun development[CONFIG_COUNT];
    EvidenceRun evaluation[CONFIG_COUNT];
    uint64_t split_ms;
    uint64_t evaluation_start_ms;
} EvidenceAnalysis;

typedef struct {
    uint64_t rows;
    uint64_t first_ms;
    uint64_t last_ms;
} Scan;

static void usage(FILE *stream) {
    fputs("usage: ./l1_evidence analyze ABSOLUTE_GUEST_PATH "
          "--source synthetic-fixture|real-guest-v1\n", stream);
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

static bool scan_row(const L1TraceRow *row, void *context) {
    Scan *scan = context;
    if (scan->rows == 0) scan->first_ms = row->relative_ms;
    scan->last_ms = row->relative_ms;
    scan->rows++;
    return true;
}

static void run_init(EvidenceRun *run, L1ForecastThresholds thresholds) {
    memset(run, 0, sizeof(*run));
    l1_shadow_init(&run->shadow, thresholds);
    l1_init(&run->published);
    l1_init(&run->reference);
}

static void score_one(bool prediction, bool outcome, uint64_t *tp,
                      uint64_t *tn, uint64_t *fp, uint64_t *fn) {
    if (prediction && outcome) (*tp)++;
    else if (!prediction && !outcome) (*tn)++;
    else if (prediction) (*fp)++;
    else (*fn)++;
}

static bool prediction_correct(bool prediction, bool outcome) {
    return prediction == outcome;
}

static void score_feedback(EvidenceRun *run, L1Feedback feedback,
                           uint64_t now_ms) {
    if (!run->pending) return;
    uint64_t elapsed = now_ms - run->pending_started_ms;
    run->scored++;
    run->feedback_elapsed_sum_ms += elapsed;
    if (elapsed > run->feedback_elapsed_max_ms)
        run->feedback_elapsed_max_ms = elapsed;
    if (elapsed > L1_FORECAST_HORIZON_MS) run->delayed_feedback++;
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        uint8_t bit = (uint8_t)(1U << c);
        bool static_prediction = (run->pending_static & bit) != 0;
        bool adaptive_prediction = (run->pending_adaptive & bit) != 0;
        bool outcome = (feedback.positive_mask & bit) != 0;
        ClassMetrics *metrics = &run->classes[c];
        score_one(static_prediction, outcome, &metrics->tp, &metrics->tn,
                  &metrics->fp, &metrics->fn);
        score_one(adaptive_prediction, outcome, &metrics->adaptive_tp,
                  &metrics->adaptive_tn, &metrics->adaptive_fp,
                  &metrics->adaptive_fn);
        if (!static_prediction && outcome) {
            metrics->static_miss_delay_sum_ms += elapsed;
            if (elapsed > metrics->static_miss_delay_max_ms)
                metrics->static_miss_delay_max_ms = elapsed;
        }
        if (!adaptive_prediction && outcome) {
            metrics->adaptive_miss_delay_sum_ms += elapsed;
            if (elapsed > metrics->adaptive_miss_delay_max_ms)
                metrics->adaptive_miss_delay_max_ms = elapsed;
        }
        if (run->pending_changed & bit) {
            metrics->changed++;
            bool fixed_right = prediction_correct(static_prediction, outcome);
            bool adaptive_right = prediction_correct(adaptive_prediction,
                                                      outcome);
            if (adaptive_right && !fixed_right) metrics->improved++;
            else if (fixed_right && !adaptive_right) metrics->worsened++;
        }
    }
    run->pending = false;
}

static void run_step(EvidenceRun *run, const L1TraceRow *row) {
    if (!run->have_time) {
        run->first_ms = row->relative_ms;
        run->have_time = true;
    }
    run->last_ms = row->relative_ms;
    run->observations++;

    bool regression = run->published.monotonic_initialized &&
        row->relative_ms < run->published.last_monotonic_ms;
    L1Action published = l1_step_at(&run->published, row->observation,
                                    row->relative_ms);
    L1Action reference = l1_step_at(&run->reference, row->observation,
                                    row->relative_ms);
    if (published != reference ||
        memcmp(&run->published, &run->reference, sizeof(L1Core)) != 0)
        run->action_mismatches++;
    if ((!row->observation.valid || regression) && published != L1_HOLD)
        run->fallback_violations++;

    L1Shadow before = run->shadow;
    L1ShadowStep step = l1_shadow_step_at(&run->shadow, row->observation,
                                          row->counters, row->relative_ms);
    uint64_t invalid_delta = run->shadow.discarded_invalid -
        before.discarded_invalid;
    uint64_t gap_delta = run->shadow.discarded_gap_or_late -
        before.discarded_gap_or_late;
    uint64_t reset_delta = run->shadow.discarded_reset -
        before.discarded_reset;
    run->discarded_invalid += invalid_delta;
    run->discarded_gap_late += gap_delta;
    run->discarded_reset += reset_delta;
    run->shadow_resets += run->shadow.reset_count - before.reset_count;
    if (invalid_delta || gap_delta || reset_delta) run->pending = false;

    if (step.feedback_ready) score_feedback(run, step.feedback,
                                            row->relative_ms);
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        run->classes[c].updates +=
            run->shadow.updates[c] - before.updates[c];
    }
    if (step.prediction_issued) {
        L1Prediction fixed;
        if (!l1_fixed_prediction(row->observation,
                                 run->shadow.base_thresholds, &fixed))
            return;
        run->issued++;
        run->pending = true;
        run->pending_static = fixed.positive_mask;
        run->pending_adaptive = step.prediction.positive_mask;
        run->pending_changed = (uint8_t)(fixed.positive_mask ^
                                         step.prediction.positive_mask);
        run->pending_started_ms = row->relative_ms;
        for (unsigned c = 0; c < CLASS_COUNT; ++c)
            if (run->shadow.offset[c] != 0) run->classes[c].offset_windows++;
    }
}

static void run_finish(EvidenceRun *run) {
    run->unfinished = run->pending ? 1U : 0U;
}

static bool analysis_row(const L1TraceRow *row, void *context) {
    EvidenceAnalysis *analysis = context;
    if (row->relative_ms <= analysis->split_ms) {
        for (unsigned c = 0; c < CONFIG_COUNT; ++c)
            run_step(&analysis->development[c], row);
    } else if (row->relative_ms >= analysis->evaluation_start_ms) {
        for (unsigned c = 0; c < CONFIG_COUNT; ++c)
            run_step(&analysis->evaluation[c], row);
    }
    return true;
}

static void ratio_text(char *buffer, size_t size, uint64_t numerator,
                       uint64_t denominator) {
    if (denominator == 0) (void)snprintf(buffer, size, "N/A");
    else (void)snprintf(buffer, size, "%.6f",
                        (double)numerator / (double)denominator);
}

static void print_class(const char *split, const char *config, unsigned c,
                        const ClassMetrics *m) {
    uint64_t static_n = m->tp + m->tn + m->fp + m->fn;
    uint64_t adaptive_n = m->adaptive_tp + m->adaptive_tn +
        m->adaptive_fp + m->adaptive_fn;
    uint64_t positives = m->tp + m->fn;
    uint64_t negatives = m->tn + m->fp;
    char prevalence[24], fixed_recall[24], adaptive_recall[24];
    char fixed_specificity[24], adaptive_specificity[24];
    char fixed_precision[24], adaptive_precision[24];
    char fixed_brier[24], adaptive_brier[24];
    char fixed_delay[24], adaptive_delay[24];
    ratio_text(prevalence, sizeof(prevalence), positives, static_n);
    ratio_text(fixed_recall, sizeof(fixed_recall), m->tp, positives);
    ratio_text(adaptive_recall, sizeof(adaptive_recall), m->adaptive_tp,
               m->adaptive_tp + m->adaptive_fn);
    ratio_text(fixed_specificity, sizeof(fixed_specificity), m->tn, negatives);
    ratio_text(adaptive_specificity, sizeof(adaptive_specificity),
               m->adaptive_tn, m->adaptive_tn + m->adaptive_fp);
    ratio_text(fixed_precision, sizeof(fixed_precision), m->tp, m->tp + m->fp);
    ratio_text(adaptive_precision, sizeof(adaptive_precision), m->adaptive_tp,
               m->adaptive_tp + m->adaptive_fp);
    ratio_text(fixed_brier, sizeof(fixed_brier), m->fp + m->fn, static_n);
    ratio_text(adaptive_brier, sizeof(adaptive_brier),
               m->adaptive_fp + m->adaptive_fn, adaptive_n);
    ratio_text(fixed_delay, sizeof(fixed_delay),
               m->static_miss_delay_sum_ms, m->fn);
    ratio_text(adaptive_delay, sizeof(adaptive_delay),
               m->adaptive_miss_delay_sum_ms, m->adaptive_fn);
    printf("CLASS split=%s config=%s class=%s scored=%" PRIu64
           " positives=%" PRIu64 " negatives=%" PRIu64
           " static_tp=%" PRIu64 " static_tn=%" PRIu64
           " static_fp=%" PRIu64 " static_fn=%" PRIu64
           " adaptive_tp=%" PRIu64 " adaptive_tn=%" PRIu64
           " adaptive_fp=%" PRIu64 " adaptive_fn=%" PRIu64
           " prevalence=%s static_recall=%s adaptive_recall=%s"
           " static_specificity=%s adaptive_specificity=%s"
           " static_precision=%s adaptive_precision=%s"
           " static_brier=%s adaptive_brier=%s"
           " static_miss_delay_mean_ms=%s static_miss_delay_max_ms=%" PRIu64
           " adaptive_miss_delay_mean_ms=%s"
           " adaptive_miss_delay_max_ms=%" PRIu64
           " updates=%" PRIu64 " offset_windows=%" PRIu64
           " changed=%" PRIu64 " improved=%" PRIu64
           " worsened=%" PRIu64 "\n",
           split, config, class_names[c], static_n, positives, negatives,
           m->tp, m->tn, m->fp, m->fn, m->adaptive_tp, m->adaptive_tn,
           m->adaptive_fp, m->adaptive_fn, prevalence, fixed_recall,
           adaptive_recall, fixed_specificity, adaptive_specificity,
           fixed_precision, adaptive_precision, fixed_brier, adaptive_brier,
           fixed_delay, m->static_miss_delay_max_ms, adaptive_delay,
           m->adaptive_miss_delay_max_ms, m->updates, m->offset_windows,
           m->changed, m->improved, m->worsened);
}

static void print_run(const char *split, const char *config,
                      const EvidenceRun *run) {
    char eligibility[24], elapsed[24];
    ratio_text(eligibility, sizeof(eligibility), run->scored, run->issued);
    ratio_text(elapsed, sizeof(elapsed), run->feedback_elapsed_sum_ms,
               run->scored);
    printf("WINDOWS split=%s config=%s observations=%" PRIu64
           " issued=%" PRIu64 " scored=%" PRIu64
           " eligibility=%s invalid=%" PRIu64 " gap_late=%" PRIu64
           " reset_discard=%" PRIu64 " shadow_resets=%" PRIu64
           " unfinished=%" PRIu64
           " delayed_feedback=%" PRIu64 " feedback_mean_ms=%s"
           " feedback_max_ms=%" PRIu64 " action_mismatches=%" PRIu64
           " fallback_violations=%" PRIu64 " first_ms=%" PRIu64
           " last_ms=%" PRIu64 "\n",
           split, config, run->observations, run->issued, run->scored,
           eligibility, run->discarded_invalid, run->discarded_gap_late,
           run->discarded_reset, run->shadow_resets, run->unfinished,
           run->delayed_feedback, elapsed, run->feedback_elapsed_max_ms,
           run->action_mismatches,
           run->fallback_violations, run->first_ms, run->last_ms);
    for (unsigned c = 0; c < CLASS_COUNT; ++c)
        print_class(split, config, c, &run->classes[c]);
}

static bool run_accounting(const EvidenceRun *run) {
    if (run->issued != run->scored + run->discarded_invalid +
        run->discarded_gap_late + run->discarded_reset + run->unfinished)
        return false;
    for (unsigned c = 0; c < CLASS_COUNT; ++c) {
        const ClassMetrics *m = &run->classes[c];
        if (m->tp + m->tn + m->fp + m->fn != run->scored ||
            m->adaptive_tp + m->adaptive_tn + m->adaptive_fp +
                m->adaptive_fn != run->scored ||
            m->improved + m->worsened != m->changed)
            return false;
    }
    return true;
}

static bool evidence_ready(const EvidenceAnalysis *analysis) {
    for (unsigned config = 0; config < CONFIG_COUNT; ++config) {
        const EvidenceRun *run = &analysis->evaluation[config];
        if (!run_accounting(run) || run->action_mismatches != 0 ||
            run->fallback_violations != 0 || run->scored < 30 ||
            run->issued == 0 || run->scored * 100U < run->issued * 80U)
            return false;
        for (unsigned c = 0; c < CLASS_COUNT; ++c) {
            const ClassMetrics *m = &run->classes[c];
            if (m->tp + m->fn < 5 || m->tn + m->fp < 5) return false;
        }
    }
    return true;
}

static int analyze(const char *path, const char *source) {
    bool real = strcmp(source, "real-guest-v1") == 0;
    if (!real && strcmp(source, "synthetic-fixture") != 0) {
        fputs("invalid --source value\n", stderr);
        return EXIT_FAILURE;
    }
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
    Scan scan = {0};
    L1TraceInfo verified, replayed;
    char error[160] = {0};
    if (!l1_trace_read(path, scan_row, &scan, &verified, error,
                       sizeof(error))) {
        fprintf(stderr, "trace validation failed: %s\n", error);
        return EXIT_FAILURE;
    }
    EvidenceAnalysis analysis;
    memset(&analysis, 0, sizeof(analysis));
    uint64_t duration_ms = scan.last_ms - scan.first_ms;
    uint64_t development_duration_ms = (duration_ms / 100U) * 60U +
        ((duration_ms % 100U) * 60U) / 100U;
    if (UINT64_MAX - scan.first_ms < development_duration_ms) {
        fputs("trace split overflow\n", stderr);
        return EXIT_FAILURE;
    }
    analysis.split_ms = scan.first_ms + development_duration_ms;
    if (UINT64_MAX - analysis.split_ms < L1_FORECAST_LATEST_MS) {
        fputs("trace split overflow\n", stderr);
        return EXIT_FAILURE;
    }
    analysis.evaluation_start_ms = analysis.split_ms +
        L1_FORECAST_LATEST_MS;
    for (unsigned c = 0; c < CONFIG_COUNT; ++c) {
        run_init(&analysis.development[c], configs[c]);
        run_init(&analysis.evaluation[c], configs[c]);
    }
    if (!l1_trace_read(path, analysis_row, &analysis, &replayed, error,
                       sizeof(error)) || stat(path, &after) != 0 ||
        !same_file(&before, &after) || verified.count != replayed.count ||
        verified.bytes != replayed.bytes ||
        verified.checksum != replayed.checksum) {
        fprintf(stderr, "trace replay changed or failed validation: %s\n", error);
        return EXIT_FAILURE;
    }
    for (unsigned c = 0; c < CONFIG_COUNT; ++c) {
        run_finish(&analysis.development[c]);
        run_finish(&analysis.evaluation[c]);
    }
    bool accounting = true, equivalence = true;
    for (unsigned c = 0; c < CONFIG_COUNT; ++c) {
        accounting = accounting && run_accounting(&analysis.development[c]) &&
            run_accounting(&analysis.evaluation[c]);
        equivalence = equivalence &&
            analysis.development[c].action_mismatches == 0 &&
            analysis.evaluation[c].action_mismatches == 0 &&
            analysis.development[c].fallback_violations == 0 &&
            analysis.evaluation[c].fallback_violations == 0;
    }
    puts("HMI_L1_EVIDENCE_V1 mode=read-only learning=unchanged action_coupling=none");
    printf("TRACE source=%s source_claim=caller-declared verification=complete schema=V1"
           " two_pass_identity=matched rows=%" PRIu64 " bytes=%" PRIu64
           " cadence_ms=%u checksum=%016" PRIx64 "\n",
           source, verified.count, verified.bytes, verified.cadence_ms,
           verified.checksum);
    printf("SPLIT development_end_ms=%" PRIu64
           " embargo_ms=%u evaluation_start_ms=%" PRIu64
           " horizon_ms=%u test_state=independent\n",
           analysis.split_ms, L1_FORECAST_LATEST_MS,
           analysis.evaluation_start_ms, L1_FORECAST_HORIZON_MS);
    for (unsigned c = 0; c < CONFIG_COUNT; ++c) {
        print_run("development", config_names[c], &analysis.development[c]);
        print_run("evaluation", config_names[c], &analysis.evaluation[c]);
    }
    printf("SAFETY accounting=%s core_action_byte_equivalence=%s"
           " sizeof_l1core=%zu sizeof_l1shadow=%zu state_v2_bytes=72\n",
           accounting ? "PASS" : "FAIL", equivalence ? "PASS" : "FAIL",
           sizeof(L1Core), sizeof(L1Shadow));
    bool coverage = evidence_ready(&analysis);
    printf("COVERAGE evaluation_windows=%" PRIu64
           " minimum=30 feedback_eligibility_required=0.800000"
           " class_min_positive=5 class_min_negative=5 gate=%s\n",
           analysis.evaluation[0].scored, coverage ? "PASS" : "FAIL");
    printf("EVIDENCE_TOOLING:%s\n",
           accounting && equivalence ? "GO" : "NO_GO");
    if (!real)
        puts("REAL_EVIDENCE:INSUFFICIENT reason=synthetic-fixture-only");
    else
        printf("REAL_EVIDENCE:%s\n",
               coverage && accounting && equivalence ? "READY" :
               "INSUFFICIENT");
    puts("FORECAST_TRANSFER:UNDETERMINED\nADAPTATION:NO_GO\n"
         "ACTION_COUPLING:NO_GO\nREAL_RESOURCE_BENEFIT:NOT_ESTABLISHED");
    return accounting && equivalence ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "analyze") == 0 &&
        strcmp(argv[3], "--source") == 0)
        return analyze(argv[2], argv[4]);
    usage(stderr);
    return EXIT_FAILURE;
}
