#define _POSIX_C_SOURCE 200809L
#include "l1_operational_memory.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { MAX_LINE = 512, MAX_ROWS_PER_FILE = 10000, EXPECTED_FIELDS = 16 };

static const char expected_header[] =
    "schema,row_id,machine,split,session_id,row_in_session,monotonic_ms,valid,quality,cpu_some_bp,memory_full_bp,io_full_bp,scoring_class,event_family,episode_id,exclude_reason\n";

static uint64_t decimal(const char *text, uint64_t maximum) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    CHECK(errno == 0 && text[0] != '\0' && end != text && *end == '\0');
    CHECK(text[0] != '-' && value <= maximum);
    return (uint64_t)value;
}

static L1PerceptionInputQuality quality_value(const char *text) {
    if (strcmp(text, "OK") == 0) return L1_PQ_OK;
    if (strcmp(text, "ABSENT") == 0) return L1_PQ_ABSENT;
    if (strcmp(text, "INVALID") == 0) return L1_PQ_INVALID;
    if (strcmp(text, "STALE") == 0) return L1_PQ_STALE;
    if (strcmp(text, "REGRESSED") == 0) return L1_PQ_REGRESSED;
    if (strcmp(text, "RESET") == 0) return L1_PQ_RESET;
    if (strcmp(text, "UNKNOWN") == 0) return (L1PerceptionInputQuality)99;
    CHECK(false);
    return L1_PQ_INVALID;
}

static size_t split_fields(char *line, char **fields) {
    size_t count = 0;
    char *cursor = line;
    while (count < EXPECTED_FIELDS) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    return count;
}

static size_t replay_file(const char *path, L1OperationalMemory *memory) {
    FILE *stream = fopen(path, "rb");
    CHECK(stream != NULL);
    char line[MAX_LINE];
    CHECK(fgets(line, sizeof(line), stream) != NULL);
    CHECK(strcmp(line, expected_header) == 0);
    size_t rows = 0;
    while (fgets(line, sizeof(line), stream) != NULL) {
        size_t length = strlen(line);
        CHECK(length != 0 && line[length - 1U] == '\n');
        line[length - 1U] = '\0';
        char *fields[EXPECTED_FIELDS];
        CHECK(split_fields(line, fields) == EXPECTED_FIELDS);
        CHECK(strcmp(fields[0], "HMI_L1_R22_CORPUS_V1") == 0);
        uint64_t time = decimal(fields[6], UINT64_MAX);
        bool valid = decimal(fields[7], 1) != 0;
        uint64_t cpu = decimal(fields[9], UINT16_MAX);
        uint64_t memory_pressure = decimal(fields[10], UINT16_MAX);
        uint64_t io = decimal(fields[11], UINT16_MAX);
        L1Observation observation = {
            (uint16_t)cpu, (uint16_t)memory_pressure, (uint16_t)io, valid
        };
        (void)l1_operational_memory_step(
            memory, observation, quality_value(fields[8]), time);
        CHECK(++rows <= MAX_ROWS_PER_FILE);
    }
    CHECK(ferror(stream) == 0);
    CHECK(fclose(stream) == 0);
    return rows;
}

static uint64_t fnv1a(const uint8_t *bytes, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t replay_pair(const char *train, const char *calibration,
                          L1OperationalMemory *memory) {
    l1_operational_memory_init(memory);
    size_t rows = replay_file(train, memory);
    rows += replay_file(calibration, memory);
    return rows;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s A_TRAIN A_CALIBRATION B_TRAIN B_CALIBRATION\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    L1OperationalMemory a, a_repeat, b;
    size_t a_rows = replay_pair(argv[1], argv[2], &a);
    size_t repeated_rows = replay_pair(argv[1], argv[2], &a_repeat);
    size_t b_rows = replay_pair(argv[3], argv[4], &b);
    CHECK(a_rows == 10240 && repeated_rows == a_rows && b_rows == 10240);
    CHECK(memcmp(&a, &a_repeat, sizeof(a)) == 0);
    CHECK(memcmp(a.bins, b.bins, sizeof(a.bins)) != 0);
    CHECK(a.status == L1_OM_SUPPORTED && b.status == L1_OM_SUPPORTED);

    uint8_t a_checkpoint[L1_OM_CHECKPOINT_BYTES];
    uint8_t repeated_checkpoint[L1_OM_CHECKPOINT_BYTES];
    uint8_t b_checkpoint[L1_OM_CHECKPOINT_BYTES];
    CHECK(l1_operational_memory_encode(&a, a_checkpoint,
                                       sizeof(a_checkpoint)));
    CHECK(l1_operational_memory_encode(&a_repeat, repeated_checkpoint,
                                       sizeof(repeated_checkpoint)));
    CHECK(l1_operational_memory_encode(&b, b_checkpoint,
                                       sizeof(b_checkpoint)));
    CHECK(memcmp(a_checkpoint, repeated_checkpoint,
                 sizeof(a_checkpoint)) == 0);
    CHECK(memcmp(a_checkpoint, b_checkpoint, sizeof(a_checkpoint)) != 0);

    printf("R22_CORPUS_MEMORY rows_a=%zu rows_b=%zu"
           " checkpoint_a=%016" PRIx64 " checkpoint_b=%016" PRIx64
           " accepted_a=%" PRIu32 " accepted_b=%" PRIu32
           " windows_a=%u windows_b=%u state_divergence=EXPECTED_SYNTHETIC_ONLY\n",
           a_rows, b_rows, fnv1a(a_checkpoint, sizeof(a_checkpoint)),
           fnv1a(b_checkpoint, sizeof(b_checkpoint)), a.accepted_lifetime,
           b.accepted_lifetime, a.windows_seen, b.windows_seen);
    puts("PASS: frozen train/calibration replay is deterministic and held-out was not opened");
    return EXIT_SUCCESS;
}
