#define _POSIX_C_SOURCE 200809L
#include "../l1_novelty.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_LINE = 512,
    DATA_FIELDS = 16,
    EPISODE_FIELDS = 10,
    MAX_ROWS = 8000,
    MAX_EPISODES = 64,
    MAX_FAMILIES = 8,
    NOVELTY_CANDIDATES = 15,
    GRID_VALUES = 6,
    K_VALUES = 5
};

typedef enum { CLASS_CONTEXT = 0, CLASS_NORMAL = 1, CLASS_POSITIVE = 2 } Truth;

typedef struct {
    uint64_t monotonic_ms;
    uint16_t session;
    uint16_t row;
    uint16_t cpu;
    uint16_t memory;
    uint16_t io;
    L1PerceptionInputQuality quality;
    Truth truth;
    bool valid;
    bool eligible;
    char episode[40];
    char family[32];
} EvalRow;

typedef struct {
    uint16_t session;
    uint16_t start_row;
    uint16_t end_row;
    uint16_t declared_eligible;
    Truth truth;
    char id[40];
    char family[32];
} Episode;

typedef struct {
    EvalRow rows[MAX_ROWS];
    Episode episodes[MAX_EPISODES];
    size_t row_count;
    size_t episode_count;
    uint32_t excluded_quality;
    uint32_t excluded_valid;
    uint32_t excluded_range;
    uint32_t excluded_time;
    uint32_t excluded_reason;
    uint32_t metadata_eligibility_mismatches;
} Dataset;

typedef struct {
    uint16_t cpu;
    uint16_t memory;
    uint16_t io;
    uint16_t k;
} FixedConfig;

typedef struct {
    uint32_t tp, fn, tn, fp;
    uint32_t unknown_positive, unknown_normal;
    uint32_t eligible_rows, excluded_rows;
} Metrics;

typedef struct {
    uint16_t novelty_threshold;
    FixedConfig tuned[2];
    bool strongest_fixed[2];
    uint64_t expected_fingerprint;
    bool study_invalid;
} Lock;

static const char data_header[] =
    "schema,row_id,machine,split,session_id,row_in_session,monotonic_ms,valid,quality,cpu_some_bp,memory_full_bp,io_full_bp,scoring_class,event_family,episode_id,exclude_reason\n";
static const char episode_header[] =
    "schema,machine,split,session_id,episode_id,scoring_class,event_family,start_row,end_row,eligible_rows\n";
static const uint16_t novelty_values[NOVELTY_CANDIDATES] = {
    256,384,512,640,704,768,832,896,928,960,976,992,1008,1016,1024
};
static const uint16_t cpu_values[GRID_VALUES] = {1000,2000,3000,4500,6000,8000};
static const uint16_t memory_values[GRID_VALUES] = {25,50,100,200,400,800};
static const uint16_t io_values[GRID_VALUES] = {250,500,1000,2000,3000,5000};
static const uint16_t k_values[K_VALUES] = {1,3,5,8,12};
static const FixedConfig original_fixed = {6000, 400, 2000, 5};

static void fail(const char *message, const char *path) {
    fprintf(stderr, "R23_ERROR %s: %s\n", message, path == NULL ? "" : path);
    exit(EXIT_FAILURE);
}

static uint64_t decimal(const char *text, uint64_t maximum, const char *path) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || text[0] == '-' || end == text ||
        *end != '\0' || value > maximum) {
        fail("invalid decimal", path);
    }
    return (uint64_t)value;
}

static size_t split_fields(char *line, char **fields, size_t expected,
                           const char *path) {
    size_t count = 0;
    char *cursor = line;
    while (count < expected) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) break;
        *comma = '\0';
        cursor = comma + 1;
    }
    if (count != expected || strchr(fields[expected - 1U], ',') != NULL) {
        fail("wrong CSV field count", path);
    }
    return count;
}

static void copy_field(char *destination, size_t size, const char *source,
                       bool allow_empty, const char *path) {
    size_t length = strlen(source);
    if ((!allow_empty && length == 0U) || length >= size) {
        fail("invalid bounded text field", path);
    }
    memcpy(destination, source, length + 1U);
}

static L1PerceptionInputQuality parse_quality(const char *text,
                                               const char *path) {
    if (strcmp(text, "OK") == 0) return L1_PQ_OK;
    if (strcmp(text, "ABSENT") == 0) return L1_PQ_ABSENT;
    if (strcmp(text, "INVALID") == 0) return L1_PQ_INVALID;
    if (strcmp(text, "STALE") == 0) return L1_PQ_STALE;
    if (strcmp(text, "REGRESSED") == 0) return L1_PQ_REGRESSED;
    if (strcmp(text, "RESET") == 0) return L1_PQ_RESET;
    if (strcmp(text, "UNKNOWN") == 0) return (L1PerceptionInputQuality)99;
    fail("unknown quality", path);
    return L1_PQ_INVALID;
}

static Truth parse_truth(const char *text, const char *path) {
    if (strcmp(text, "context") == 0) return CLASS_CONTEXT;
    if (strcmp(text, "normal") == 0) return CLASS_NORMAL;
    if (strcmp(text, "positive") == 0) return CLASS_POSITIVE;
    fail("unknown scoring class", path);
    return CLASS_CONTEXT;
}

static bool read_line(FILE *stream, char line[MAX_LINE], const char *path) {
    if (fgets(line, MAX_LINE, stream) == NULL) return false;
    size_t length = strlen(line);
    if (length == 0U || line[length - 1U] != '\n') {
        fail("truncated or oversized line", path);
    }
    line[length - 1U] = '\0';
    return true;
}

static void train_reference(const char *path, char machine,
                            L1OperationalMemory *memory,
                            uint64_t *checkpoint_fingerprint) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) fail("cannot open training file", path);
    char line[MAX_LINE];
    if (fgets(line, sizeof(line), stream) == NULL ||
        strcmp(line, data_header) != 0) fail("bad training header", path);
    l1_operational_memory_init(memory);
    size_t rows = 0;
    while (read_line(stream, line, path)) {
        char *field[DATA_FIELDS];
        split_fields(line, field, DATA_FIELDS, path);
        if (strcmp(field[0], "HMI_L1_R22_CORPUS_V1") != 0 ||
            field[2][0] != machine || field[2][1] != '\0' ||
            strcmp(field[3], "train") != 0) fail("training identity mismatch", path);
        uint16_t cpu = (uint16_t)decimal(field[9], UINT16_MAX, path);
        uint16_t mem = (uint16_t)decimal(field[10], UINT16_MAX, path);
        uint16_t io = (uint16_t)decimal(field[11], UINT16_MAX, path);
        L1Observation observation = {cpu, mem, io,
            decimal(field[7], 1, path) != 0U};
        (void)l1_operational_memory_step(memory, observation,
            parse_quality(field[8], path), decimal(field[6], UINT64_MAX, path));
        if (++rows > MAX_ROWS) fail("too many training rows", path);
    }
    if (ferror(stream) != 0 || fclose(stream) != 0 || rows != 7680U ||
        memory->status != L1_OM_SUPPORTED) fail("unsupported training reference", path);
    uint8_t checkpoint[L1_OM_CHECKPOINT_BYTES];
    if (!l1_operational_memory_encode(memory, checkpoint, sizeof(checkpoint)))
        fail("training checkpoint encode failed", path);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof(checkpoint); ++i) {
        hash ^= checkpoint[i];
        hash *= UINT64_C(1099511628211);
    }
    *checkpoint_fingerprint = hash;
}

static void load_rows(const char *path, char machine, const char *split,
                      Dataset *dataset) {
    memset(dataset, 0, sizeof(*dataset));
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) fail("cannot open evaluation data", path);
    char line[MAX_LINE];
    if (fgets(line, sizeof(line), stream) == NULL ||
        strcmp(line, data_header) != 0) fail("bad data header", path);
    bool session_seen = false;
    uint16_t prior_session = 0, prior_row = 0;
    uint64_t prior_time = 0;
    while (read_line(stream, line, path)) {
        if (dataset->row_count == MAX_ROWS) fail("too many rows", path);
        EvalRow *row = &dataset->rows[dataset->row_count++];
        char *field[DATA_FIELDS];
        split_fields(line, field, DATA_FIELDS, path);
        if (strcmp(field[0], "HMI_L1_R22_CORPUS_V1") != 0 ||
            field[1][0] == '\0' || field[2][0] != machine ||
            field[2][1] != '\0' || strcmp(field[3], split) != 0) {
            fail("data identity mismatch", path);
        }
        row->session = (uint16_t)decimal(field[4], UINT16_MAX, path);
        row->row = (uint16_t)decimal(field[5], 319, path);
        row->monotonic_ms = decimal(field[6], UINT64_MAX, path);
        row->valid = decimal(field[7], 1, path) != 0U;
        row->quality = parse_quality(field[8], path);
        row->cpu = (uint16_t)decimal(field[9], UINT16_MAX, path);
        row->memory = (uint16_t)decimal(field[10], UINT16_MAX, path);
        row->io = (uint16_t)decimal(field[11], UINT16_MAX, path);
        row->truth = parse_truth(field[12], path);
        copy_field(row->family, sizeof(row->family), field[13], false, path);
        copy_field(row->episode, sizeof(row->episode), field[14],
                   row->truth == CLASS_CONTEXT, path);
        bool identity_order = !session_seen || row->session > prior_session ||
            (row->session == prior_session && row->row == prior_row + 1U);
        if (!identity_order) fail("row/session order mismatch", path);
        bool time_ok = !session_seen || row->session != prior_session ||
            row->monotonic_ms > prior_time;
        row->eligible = true;
        if (row->quality != L1_PQ_OK) {
            row->eligible = false; ++dataset->excluded_quality;
        } else if (!row->valid) {
            row->eligible = false; ++dataset->excluded_valid;
        } else if (row->cpu > 10000U || row->memory > 10000U || row->io > 10000U) {
            row->eligible = false; ++dataset->excluded_range;
        } else if (!time_ok) {
            row->eligible = false; ++dataset->excluded_time;
        } else if (field[15][0] != '\0') {
            row->eligible = false; ++dataset->excluded_reason;
        }
        session_seen = true;
        prior_session = row->session;
        prior_row = row->row;
        prior_time = row->monotonic_ms;
    }
    if (ferror(stream) != 0 || fclose(stream) != 0 || dataset->row_count != 2560U)
        fail("wrong evaluation row count", path);
}

static void load_episodes(const char *path, char machine, const char *split,
                          Dataset *dataset) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) fail("cannot open episode data", path);
    char line[MAX_LINE];
    if (fgets(line, sizeof(line), stream) == NULL ||
        strcmp(line, episode_header) != 0) fail("bad episode header", path);
    while (read_line(stream, line, path)) {
        if (dataset->episode_count == MAX_EPISODES) fail("too many episodes", path);
        Episode *episode = &dataset->episodes[dataset->episode_count++];
        char *field[EPISODE_FIELDS];
        split_fields(line, field, EPISODE_FIELDS, path);
        if (strcmp(field[0], "HMI_L1_R22_CORPUS_V1") != 0 ||
            field[1][0] != machine || field[1][1] != '\0' ||
            strcmp(field[2], split) != 0) fail("episode identity mismatch", path);
        episode->session = (uint16_t)decimal(field[3], UINT16_MAX, path);
        copy_field(episode->id, sizeof(episode->id), field[4], false, path);
        episode->truth = parse_truth(field[5], path);
        if (episode->truth == CLASS_CONTEXT) fail("context listed as episode", path);
        copy_field(episode->family, sizeof(episode->family), field[6], false, path);
        episode->start_row = (uint16_t)decimal(field[7], 319, path);
        episode->end_row = (uint16_t)decimal(field[8], 319, path);
        episode->declared_eligible = (uint16_t)decimal(field[9], 320, path);
        if (episode->end_row < episode->start_row) fail("episode range reversed", path);
    }
    if (ferror(stream) != 0 || fclose(stream) != 0 || dataset->episode_count != 56U)
        fail("wrong episode count", path);

    for (size_t e = 0; e < dataset->episode_count; ++e) {
        const Episode *episode = &dataset->episodes[e];
        uint16_t seen = 0, eligible = 0;
        for (size_t r = 0; r < dataset->row_count; ++r) {
            const EvalRow *row = &dataset->rows[r];
            if (row->session == episode->session && row->row >= episode->start_row &&
                row->row <= episode->end_row) {
                if (strcmp(row->episode, episode->id) != 0 ||
                    strcmp(row->family, episode->family) != 0 ||
                    row->truth != episode->truth) fail("episode/data mismatch", path);
                ++seen;
                if (row->eligible) ++eligible;
            }
        }
        uint16_t span = (uint16_t)(episode->end_row - episode->start_row + 1U);
        if (seen != span || episode->declared_eligible != span)
            fail("episode range/declared count mismatch", path);
        if (eligible != episode->declared_eligible)
            ++dataset->metadata_eligibility_mismatches;
    }
}

typedef enum { MODEL_NOVELTY, MODEL_FIXED } ModelKind;

static bool row_hit(const EvalRow *row, ModelKind kind,
                    const L1OperationalMemorySummary *reference,
                    uint16_t novelty_threshold, FixedConfig fixed,
                    bool *known) {
    *known = row->eligible;
    if (!row->eligible) return false;
    if (kind == MODEL_FIXED) {
        return row->cpu >= fixed.cpu || row->memory >= fixed.memory ||
               row->io >= fixed.io;
    }
    L1Observation observation = {row->cpu, row->memory, row->io, row->valid};
    L1NoveltyResult result = l1_novelty_score(
        reference, observation, row->quality, novelty_threshold);
    if (result.disposition == L1_NOVELTY_UNKNOWN) {
        *known = false;
        return false;
    }
    return result.disposition == L1_NOVELTY_SUSPECTED;
}

static int episode_decision(const Dataset *dataset, const Episode *episode,
                            ModelKind kind,
                            const L1OperationalMemorySummary *reference,
                            uint16_t novelty_threshold, FixedConfig fixed,
                            uint32_t *eligible_rows, uint32_t *hits) {
    *eligible_rows = 0; *hits = 0;
    for (size_t r = 0; r < dataset->row_count; ++r) {
        const EvalRow *row = &dataset->rows[r];
        if (row->session != episode->session || row->row < episode->start_row ||
            row->row > episode->end_row) continue;
        bool known = false;
        bool hit = row_hit(row, kind, reference, novelty_threshold, fixed, &known);
        if (known) {
            ++*eligible_rows;
            if (hit) ++*hits;
        }
    }
    if (*eligible_rows < 10U) return -1;
    uint16_t required = kind == MODEL_NOVELTY ? 5U : fixed.k;
    return *hits >= required ? 1 : 0;
}

static Metrics evaluate(const Dataset *dataset, ModelKind kind,
                        const L1OperationalMemorySummary *reference,
                        uint16_t novelty_threshold, FixedConfig fixed) {
    Metrics metrics = {0};
    for (size_t e = 0; e < dataset->episode_count; ++e) {
        const Episode *episode = &dataset->episodes[e];
        uint32_t eligible, hits;
        int decision = episode_decision(dataset, episode, kind, reference,
                                        novelty_threshold, fixed, &eligible, &hits);
        metrics.eligible_rows += eligible;
        metrics.excluded_rows += (uint32_t)(episode->end_row -
            episode->start_row + 1U) - eligible;
        if (decision < 0) {
            if (episode->truth == CLASS_POSITIVE) ++metrics.unknown_positive;
            else ++metrics.unknown_normal;
        } else if (episode->truth == CLASS_POSITIVE) {
            if (decision != 0) ++metrics.tp; else ++metrics.fn;
        } else {
            if (decision != 0) ++metrics.fp; else ++metrics.tn;
        }
    }
    return metrics;
}

static bool cap_met(Metrics metrics) {
    uint32_t normal = metrics.fp + metrics.tn;
    return normal != 0U && metrics.fp * 10U <= normal;
}

static bool tuned_better(Metrics candidate, FixedConfig config,
                         Metrics best, FixedConfig best_config,
                         bool have_best) {
    if (!have_best) return true;
    bool candidate_cap = cap_met(candidate), best_cap = cap_met(best);
    if (candidate_cap != best_cap) return candidate_cap;
    if (candidate_cap) {
        if (candidate.tp != best.tp) return candidate.tp > best.tp;
        if (candidate.fp != best.fp) return candidate.fp < best.fp;
    } else {
        if (candidate.fp != best.fp) return candidate.fp < best.fp;
        if (candidate.tp != best.tp) return candidate.tp > best.tp;
    }
    if (config.cpu != best_config.cpu) return config.cpu < best_config.cpu;
    if (config.memory != best_config.memory) return config.memory < best_config.memory;
    if (config.io != best_config.io) return config.io < best_config.io;
    return config.k < best_config.k;
}

static FixedConfig tune_fixed(const Dataset *dataset, Metrics *selected) {
    FixedConfig best = {0};
    Metrics best_metrics = {0};
    bool have = false;
    for (size_t c = 0; c < GRID_VALUES; ++c)
    for (size_t m = 0; m < GRID_VALUES; ++m)
    for (size_t i = 0; i < GRID_VALUES; ++i)
    for (size_t k = 0; k < K_VALUES; ++k) {
        FixedConfig config = {cpu_values[c], memory_values[m], io_values[i], k_values[k]};
        Metrics metrics = evaluate(dataset, MODEL_FIXED, NULL, 0, config);
        if (tuned_better(metrics, config, best_metrics, best, have)) {
            best = config; best_metrics = metrics; have = true;
        }
    }
    if (!have) fail("empty comparator search", NULL);
    *selected = best_metrics;
    return best;
}

static uint32_t ratio_scaled(uint32_t numerator, uint32_t denominator) {
    return denominator == 0U ? 0U : (uint32_t)((uint64_t)numerator * 1000000U / denominator);
}

static bool novelty_better(Metrics a, Metrics b, uint16_t threshold,
                           Metrics best_a, Metrics best_b,
                           uint16_t best_threshold, bool have) {
    if (!have) return true;
    uint32_t a_pos = a.tp + a.fn, b_pos = b.tp + b.fn;
    uint32_t ba_pos = best_a.tp + best_a.fn, bb_pos = best_b.tp + best_b.fn;
    uint32_t min_s = ratio_scaled(a.tp, a_pos);
    uint32_t value = ratio_scaled(b.tp, b_pos);
    if (value < min_s) min_s = value;
    uint32_t best_min = ratio_scaled(best_a.tp, ba_pos);
    value = ratio_scaled(best_b.tp, bb_pos);
    if (value < best_min) best_min = value;
    if (min_s != best_min) return min_s > best_min;
    uint32_t sum_s = ratio_scaled(a.tp, a_pos) + ratio_scaled(b.tp, b_pos);
    uint32_t best_sum = ratio_scaled(best_a.tp, ba_pos) + ratio_scaled(best_b.tp, bb_pos);
    if (sum_s != best_sum) return sum_s > best_sum;
    uint32_t a_norm = a.fp + a.tn, b_norm = b.fp + b.tn;
    uint32_t ba_norm = best_a.fp + best_a.tn, bb_norm = best_b.fp + best_b.tn;
    uint32_t max_f = ratio_scaled(a.fp, a_norm);
    value = ratio_scaled(b.fp, b_norm); if (value > max_f) max_f = value;
    uint32_t best_max = ratio_scaled(best_a.fp, ba_norm);
    value = ratio_scaled(best_b.fp, bb_norm); if (value > best_max) best_max = value;
    if (max_f != best_max) return max_f < best_max;
    uint32_t sum_f = ratio_scaled(a.fp, a_norm) + ratio_scaled(b.fp, b_norm);
    uint32_t best_sum_f = ratio_scaled(best_a.fp, ba_norm) + ratio_scaled(best_b.fp, bb_norm);
    if (sum_f != best_sum_f) return sum_f < best_sum_f;
    return threshold > best_threshold;
}

static const char *decision_name(int decision) {
    if (decision < 0) return "UNKNOWN";
    return decision != 0 ? "SUSPECTED" : "EXPECTED";
}

static void print_metrics(const char *phase, char machine, const char *model,
                          Metrics metrics) {
    uint32_t positives = metrics.tp + metrics.fn;
    uint32_t normals = metrics.fp + metrics.tn;
    printf("METRIC phase=%s machine=%c model=%s tp=%" PRIu32
           " fn=%" PRIu32 " tn=%" PRIu32 " fp=%" PRIu32
           " unknown_positive=%" PRIu32 " unknown_normal=%" PRIu32
           " sensitivity_ppm=%" PRIu32 " false_alert_ppm=%" PRIu32
           " eligible_rows=%" PRIu32 " excluded_rows=%" PRIu32 "\n",
           phase, machine, model, metrics.tp, metrics.fn, metrics.tn,
           metrics.fp, metrics.unknown_positive, metrics.unknown_normal,
           ratio_scaled(metrics.tp, positives), ratio_scaled(metrics.fp, normals),
           metrics.eligible_rows, metrics.excluded_rows);
}

static bool strongest_is_fixed(Metrics fixed, Metrics tuned) {
    bool fixed_cap = cap_met(fixed), tuned_cap = cap_met(tuned);
    if (fixed_cap != tuned_cap) return fixed_cap;
    if (!fixed_cap) return false;
    if (fixed.tp != tuned.tp) return fixed.tp > tuned.tp;
    if (fixed.fp != tuned.fp) return fixed.fp < tuned.fp;
    return true;
}

static void calibration(const char *corpus) {
    char path[512];
    L1OperationalMemory memories[2];
    L1OperationalMemorySummary references[2];
    Dataset datasets[2];
    uint64_t checkpoint[2];
    const char machines[2] = {'A','B'};
    for (size_t index = 0; index < 2; ++index) {
        snprintf(path, sizeof(path), "%s/machine_%c_train.csv", corpus,
                 (char)('a' + index));
        train_reference(path, machines[index], &memories[index], &checkpoint[index]);
        references[index] = l1_operational_memory_summary(&memories[index]);
        snprintf(path, sizeof(path), "%s/machine_%c_calibration.csv", corpus,
                 (char)('a' + index));
        load_rows(path, machines[index], "calibration", &datasets[index]);
        snprintf(path, sizeof(path), "%s/machine_%c_calibration_episodes.csv", corpus,
                 (char)('a' + index));
        load_episodes(path, machines[index], "calibration", &datasets[index]);
    }

    Metrics novelty_selected[2] = {{0}};
    uint16_t selected_threshold = 1024;
    bool have_novelty = false;
    for (size_t n = 0; n < NOVELTY_CANDIDATES; ++n) {
        Metrics a = evaluate(&datasets[0], MODEL_NOVELTY, &references[0],
                             novelty_values[n], original_fixed);
        Metrics b = evaluate(&datasets[1], MODEL_NOVELTY, &references[1],
                             novelty_values[n], original_fixed);
        bool support = a.tp + a.fn >= 10U && a.fp + a.tn >= 10U &&
                       b.tp + b.fn >= 10U && b.fp + b.tn >= 10U;
        if (support && cap_met(a) && cap_met(b) &&
            novelty_better(a, b, novelty_values[n], novelty_selected[0],
                           novelty_selected[1], selected_threshold, have_novelty)) {
            novelty_selected[0] = a; novelty_selected[1] = b;
            selected_threshold = novelty_values[n]; have_novelty = true;
        }
    }
    if (!have_novelty) {
        selected_threshold = 1024;
        novelty_selected[0] = evaluate(&datasets[0], MODEL_NOVELTY,
            &references[0], selected_threshold, original_fixed);
        novelty_selected[1] = evaluate(&datasets[1], MODEL_NOVELTY,
            &references[1], selected_threshold, original_fixed);
    }

    FixedConfig tuned[2]; Metrics tuned_metrics[2], fixed_metrics[2];
    bool strongest_fixed[2];
    for (size_t index = 0; index < 2; ++index) {
        tuned[index] = tune_fixed(&datasets[index], &tuned_metrics[index]);
        fixed_metrics[index] = evaluate(&datasets[index], MODEL_FIXED, NULL, 0,
                                        original_fixed);
        strongest_fixed[index] = strongest_is_fixed(fixed_metrics[index],
                                                     tuned_metrics[index]);
        print_metrics("calibration", machines[index], "novelty",
                      novelty_selected[index]);
        print_metrics("calibration", machines[index], "fixed-original",
                      fixed_metrics[index]);
        print_metrics("calibration", machines[index], "fixed-tuned",
                      tuned_metrics[index]);
        printf("EXCLUSIONS phase=calibration machine=%c quality=%" PRIu32
               " valid=%" PRIu32 " range=%" PRIu32 " time=%" PRIu32
               " reason=%" PRIu32 " metadata_eligibility_mismatches=%" PRIu32
               "\n", machines[index],
               datasets[index].excluded_quality, datasets[index].excluded_valid,
               datasets[index].excluded_range, datasets[index].excluded_time,
               datasets[index].excluded_reason,
               datasets[index].metadata_eligibility_mismatches);
    }
    printf("CALIBRATION_LOCK_VALUES novelty_threshold=%u novelty_cap=%s"
           " a_tuned=%u|%u|%u|%u a_tuned_cap=%s a_strongest=%s"
           " b_tuned=%u|%u|%u|%u b_tuned_cap=%s b_strongest=%s"
           " checkpoint_a=%016" PRIx64 " checkpoint_b=%016" PRIx64 "\n",
           selected_threshold, have_novelty ? "PASS" : "CALIBRATION_CAP_NOT_MET",
           tuned[0].cpu, tuned[0].memory, tuned[0].io, tuned[0].k,
           cap_met(tuned_metrics[0]) ? "PASS" : "CALIBRATION_CAP_NOT_MET",
           strongest_fixed[0] ? "fixed-original" : "fixed-tuned",
           tuned[1].cpu, tuned[1].memory, tuned[1].io, tuned[1].k,
           cap_met(tuned_metrics[1]) ? "PASS" : "CALIBRATION_CAP_NOT_MET",
           strongest_fixed[1] ? "fixed-original" : "fixed-tuned",
           checkpoint[0], checkpoint[1]);
}

static uint64_t fnv_update(uint64_t hash, const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}

static bool valid_hex64(const char *text) {
    if (strlen(text) != 64U) return false;
    for (size_t i = 0; i < 64U; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

static bool contains_u16(const uint16_t *values, size_t count, uint16_t value) {
    for (size_t i = 0; i < count; ++i) if (values[i] == value) return true;
    return false;
}

static Lock read_lock(const char *path) {
    Lock lock = {0};
    bool seen_schema=false, seen_corpus=false, seen_source=false, seen_eval=false;
    bool seen_threshold=false, seen_a=false, seen_b=false, seen_as=false, seen_bs=false;
    bool seen_study=false;
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) fail("cannot open calibration lock", path);
    char line[MAX_LINE];
    uint64_t hash = UINT64_C(1469598103934665603);
    bool fingerprint_seen = false;
    while (fgets(line, sizeof(line), stream) != NULL) {
        size_t length = strlen(line);
        if (length == 0U || line[length - 1U] != '\n') fail("bad lock line", path);
        if (strncmp(line, "lock_fingerprint=", 17) == 0) {
            if (fingerprint_seen) fail("duplicate lock fingerprint", path);
            char *end = NULL; errno = 0;
            unsigned long long value = strtoull(line + 17, &end, 16);
            if (errno != 0 || end != line + length - 1U) fail("bad lock fingerprint", path);
            lock.expected_fingerprint = value; fingerprint_seen = true;
            continue;
        }
        if (fingerprint_seen) fail("fingerprint must be final", path);
        hash = fnv_update(hash, (const uint8_t *)line, length);
        line[length - 1U] = '\0';
        char *equals = strchr(line, '=');
        if (equals == NULL || strchr(equals + 1, '=') != NULL) fail("bad lock field", path);
        *equals = '\0'; const char *value = equals + 1;
        if (strcmp(line, "schema") == 0) {
            if (seen_schema || strcmp(value, "HMI_L1_R23_CALIBRATION_LOCK_V1") != 0) fail("bad lock schema", path);
            seen_schema=true;
        } else if (strcmp(line, "corpus_sha256") == 0) {
            if (seen_corpus || strcmp(value, "f83a7bda572b6319554397ce780a11fa4bc0ffc698249013cedb47ec4d208963") != 0) fail("bad corpus identity", path);
            seen_corpus=true;
        } else if (strcmp(line, "study_status") == 0) {
            if (seen_study || (strcmp(value, "VALID") != 0 &&
                strcmp(value, "INVALID_EPISODE_ELIGIBILITY_METADATA") != 0))
                fail("bad study status", path);
            lock.study_invalid = strcmp(value, "VALID") != 0;
            seen_study = true;
        } else if (strcmp(line, "novelty_source_sha256") == 0) {
            if (seen_source || !valid_hex64(value)) fail("bad source identity", path);
            seen_source=true;
        } else if (strcmp(line, "evaluator_source_sha256") == 0) {
            if (seen_eval || !valid_hex64(value)) fail("bad evaluator identity", path);
            seen_eval=true;
        } else if (strcmp(line, "novelty_threshold") == 0) {
            if (seen_threshold) fail("duplicate threshold", path);
            lock.novelty_threshold=(uint16_t)decimal(value,1024,path); seen_threshold=true;
        } else if (strcmp(line, "novelty_cap") == 0 ||
                   strcmp(line, "machine_a_tuned_cap") == 0 ||
                   strcmp(line, "machine_b_tuned_cap") == 0) {
            if (strcmp(value, "PASS") != 0 &&
                strcmp(value, "CALIBRATION_CAP_NOT_MET") != 0)
                fail("bad calibration cap status", path);
        } else if (strcmp(line, "machine_a_tuned") == 0 || strcmp(line, "machine_b_tuned") == 0) {
            size_t index = line[8] == 'a' ? 0U : 1U;
            if ((index == 0U && seen_a) || (index == 1U && seen_b)) fail("duplicate tuned config", path);
            unsigned c,m,i,k; char tail;
            if (sscanf(value, "%u|%u|%u|%u%c", &c,&m,&i,&k,&tail) != 4 ||
                c>UINT16_MAX || m>UINT16_MAX || i>UINT16_MAX || k>UINT16_MAX)
                fail("bad tuned config", path);
            lock.tuned[index]=(FixedConfig){(uint16_t)c,(uint16_t)m,(uint16_t)i,(uint16_t)k};
            if (index==0U) seen_a=true; else seen_b=true;
        } else if (strcmp(line, "machine_a_strongest") == 0 || strcmp(line, "machine_b_strongest") == 0) {
            size_t index = line[8] == 'a' ? 0U : 1U;
            if ((index==0U && seen_as) || (index==1U && seen_bs) ||
                (strcmp(value,"fixed-original")!=0 && strcmp(value,"fixed-tuned")!=0))
                fail("bad strongest config", path);
            lock.strongest_fixed[index]=strcmp(value,"fixed-original")==0;
            if (index==0U) seen_as=true; else seen_bs=true;
        } else if (strncmp(line, "identity_", 9)==0 || strncmp(line,"calibration_",12)==0 ||
                   strncmp(line,"checkpoint_",11)==0 || strcmp(line,"selection_version")==0) {
            if (value[0]=='\0') fail("empty lock evidence", path);
        } else {
            fail("unknown lock key", path);
        }
    }
    if (ferror(stream)!=0 || fclose(stream)!=0 || !fingerprint_seen || hash!=lock.expected_fingerprint ||
        !seen_schema || !seen_corpus || !seen_study || !seen_source || !seen_eval || !seen_threshold ||
        !seen_a || !seen_b || !seen_as || !seen_bs) fail("incomplete or unbound lock", path);
    if (!contains_u16(novelty_values, NOVELTY_CANDIDATES,
                      lock.novelty_threshold)) fail("threshold outside frozen set", path);
    for (size_t index = 0; index < 2; ++index) {
        if (!contains_u16(cpu_values, GRID_VALUES, lock.tuned[index].cpu) ||
            !contains_u16(memory_values, GRID_VALUES, lock.tuned[index].memory) ||
            !contains_u16(io_values, GRID_VALUES, lock.tuned[index].io) ||
            !contains_u16(k_values, K_VALUES, lock.tuned[index].k)) {
            fail("tuned config outside frozen grid", path);
        }
    }
    return lock;
}

typedef struct { char name[32]; Metrics metrics[3]; uint32_t episodes; } Family;
typedef struct { bool support; bool noninferior; bool family_win; } MachineGate;

static size_t family_index(Family families[MAX_FAMILIES], size_t *count,
                           const char *name) {
    for (size_t i=0;i<*count;++i) if(strcmp(families[i].name,name)==0) return i;
    if (*count==MAX_FAMILIES) fail("too many families", name);
    copy_field(families[*count].name,sizeof(families[*count].name),name,false,name);
    return (*count)++;
}

static MachineGate heldout_machine(const char *corpus, const Lock *lock,
                                   size_t index) {
    char path[512]; char machine=(char)('A'+index), lower=(char)('a'+index);
    L1OperationalMemory memory; uint64_t checkpoint;
    snprintf(path,sizeof(path),"%s/machine_%c_train.csv",corpus,lower);
    train_reference(path,machine,&memory,&checkpoint);
    L1OperationalMemorySummary reference=l1_operational_memory_summary(&memory);
    Dataset dataset;
    snprintf(path,sizeof(path),"%s/machine_%c_heldout.csv",corpus,lower);
    load_rows(path,machine,"heldout",&dataset);
    snprintf(path,sizeof(path),"%s/machine_%c_heldout_episodes.csv",corpus,lower);
    load_episodes(path,machine,"heldout",&dataset);
    FixedConfig configs[3]={{0},original_fixed,lock->tuned[index]};
    const char *names[3]={"novelty","fixed-original","fixed-tuned"};
    Metrics total[3]={{0}}; Family families[MAX_FAMILIES]; size_t family_count=0;
    memset(families, 0, sizeof(families));
    for(size_t e=0;e<dataset.episode_count;++e){
        const Episode *episode=&dataset.episodes[e];
        size_t fi=family_index(families,&family_count,episode->family); ++families[fi].episodes;
        int decisions[3]; uint32_t eligible=0,hits[3]={0};
        for(size_t model=0;model<3;++model){
            uint32_t model_eligible;
            decisions[model]=episode_decision(&dataset,episode,
                model==0?MODEL_NOVELTY:MODEL_FIXED,&reference,lock->novelty_threshold,
                configs[model],&model_eligible,&hits[model]);
            if(model==0) eligible=model_eligible; else if(model_eligible!=eligible) fail("coverage mismatch",episode->id);
            Metrics *targets[2]={&total[model],&families[fi].metrics[model]};
            for(size_t t=0;t<2;++t){
                targets[t]->eligible_rows+=eligible;
                targets[t]->excluded_rows+=(uint32_t)(episode->end_row-episode->start_row+1U)-eligible;
                if(decisions[model]<0){if(episode->truth==CLASS_POSITIVE)++targets[t]->unknown_positive;else++targets[t]->unknown_normal;}
                else if(episode->truth==CLASS_POSITIVE){if(decisions[model])++targets[t]->tp;else++targets[t]->fn;}
                else {if(decisions[model])++targets[t]->fp;else++targets[t]->tn;}
            }
        }
        printf("EPISODE machine=%c id=%s family=%s truth=%s eligible=%" PRIu32
               " novelty=%s novelty_hits=%" PRIu32 " fixed_original=%s fixed_original_hits=%" PRIu32
               " fixed_tuned=%s fixed_tuned_hits=%" PRIu32 "\n",machine,episode->id,episode->family,
               episode->truth==CLASS_POSITIVE?"positive":"normal",eligible,
               decision_name(decisions[0]),hits[0],decision_name(decisions[1]),hits[1],
               decision_name(decisions[2]),hits[2]);
    }
    for(size_t model=0;model<3;++model) print_metrics("heldout",machine,names[model],total[model]);
    for(size_t f=0;f<family_count;++f) for(size_t model=0;model<3;++model){
        printf("FAMILY name=%s episodes=%" PRIu32 " ",families[f].name,families[f].episodes);
        print_metrics("heldout",machine,names[model],families[f].metrics[model]);
    }
    size_t strongest=lock->strongest_fixed[index]?1U:2U;
    bool support=total[0].tp+total[0].fn>=20U && total[0].fp+total[0].tn>=20U &&
                 total[0].unknown_positive==0U && total[0].unknown_normal==0U;
    bool noninferior=total[0].fp<=total[strongest].fp && total[0].tp>=total[strongest].tp;
    bool family_win=false;
    for(size_t f=0;f<family_count;++f){Metrics n=families[f].metrics[0],s=families[f].metrics[strongest];
        if(n.tp+n.fn>=5U && n.tp>s.tp) family_win=true;
    }
    printf("MACHINE_GATE machine=%c support=%s strongest=%s noninferior=%s family_win=%s"
           " checkpoint=%016" PRIx64 " exclusions=%" PRIu32 "|%" PRIu32 "|%" PRIu32 "|%" PRIu32 "|%" PRIu32
           " metadata_eligibility_mismatches=%" PRIu32 "\n",
           machine,support?"PASS":"FAIL",names[strongest],noninferior?"PASS":"FAIL",family_win?"PASS":"FAIL",checkpoint,
           dataset.excluded_quality,dataset.excluded_valid,dataset.excluded_range,dataset.excluded_time,dataset.excluded_reason,
           dataset.metadata_eligibility_mismatches);
    return (MachineGate){support, noninferior, family_win};
}

static void heldout(const char *corpus, const char *lock_path) {
    Lock lock=read_lock(lock_path);
    MachineGate a=heldout_machine(corpus,&lock,0);
    MachineGate b=heldout_machine(corpus,&lock,1);
    const char *utility;
    if (lock.study_invalid) utility="UNDETERMINED";
    else if (!a.support || !b.support) utility="INSUFFICIENT_SUPPORT";
    else if (a.noninferior && b.noninferior && (a.family_win || b.family_win)) utility="GO";
    else utility="NO_GO";
    printf("R23_HELDOUT_COMPLETE no_retuning=YES R2_3_STUDY=%s R2_UTILITY=%s"
           " R2_INDIVIDUALITY=UNDETERMINED\n",
           lock.study_invalid ? "INVALID" : "VALID", utility);
}

int main(int argc,char **argv){
    if(argc==3 && strcmp(argv[1],"calibrate")==0){calibration(argv[2]);return EXIT_SUCCESS;}
    if(argc==4 && strcmp(argv[1],"heldout")==0){heldout(argv[2],argv[3]);return EXIT_SUCCESS;}
    fprintf(stderr,"usage: %s calibrate CORPUS_DIR | heldout CORPUS_DIR CALIBRATION_LOCK\n",argv[0]);
    return EXIT_FAILURE;
}
