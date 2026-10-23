#define _POSIX_C_SOURCE 200809L
#include "l1_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const char *header_prefix[] = {
    "HMI_L1_PSI_TRACE_V1\n", "schema=1\n",
    "units=relative_ms,basis_points,microseconds\n", "cadence_ms=1000\n",
};
static const char columns[] =
    "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
    "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n";

static void write_fixture_limit(const char *path, const char *const rows[3],
                                const char *limit_kind, unsigned limit_value,
                                bool bad_hash, bool extra) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof(header_prefix) / sizeof(header_prefix[0]); ++i) {
        CHECK(fputs(header_prefix[i], file) >= 0);
        hash = l1_trace_hash_bytes(hash, header_prefix[i], strlen(header_prefix[i]));
    }
    char header[64];
    int size = snprintf(header, sizeof(header), "limit=%s\n", limit_kind);
    CHECK(size > 0 && (size_t)size < sizeof(header));
    CHECK(fputs(header, file) >= 0);
    hash = l1_trace_hash_bytes(hash, header, (size_t)size);
    size = snprintf(header, sizeof(header), "limit_value=%u\n", limit_value);
    CHECK(size > 0 && (size_t)size < sizeof(header));
    CHECK(fputs(header, file) >= 0);
    hash = l1_trace_hash_bytes(hash, header, (size_t)size);
    CHECK(fputs(columns, file) >= 0);
    hash = l1_trace_hash_bytes(hash, columns, strlen(columns));
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(fputs(rows[i], file) >= 0);
        hash = l1_trace_hash_bytes(hash, rows[i], strlen(rows[i]));
    }
    if (bad_hash) hash ^= 1U;
    CHECK(fprintf(file, "END,count=3,fnv1a64=%016" PRIx64 "\n", hash) > 0);
    if (extra) CHECK(fputs("extra\n", file) >= 0);
    CHECK(fclose(file) == 0);
}

static void write_fixture(const char *path, const char *const rows[3],
                          bool bad_hash, bool extra) {
    write_fixture_limit(path, rows, "samples", 3, bad_hash, extra);
}

static bool count_row(const L1TraceRow *row, void *context) {
    uint64_t *count = context;
    CHECK(row->observation.valid == (row->flags == 0));
    (*count)++;
    return true;
}

static void expect_invalid(const char *path, const char *const rows[3],
                           bool bad_hash, bool extra) {
    write_fixture(path, rows, bad_hash, extra);
    L1TraceInfo info;
    char error[128] = {0};
    CHECK(!l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
}

int main(void) {
    char path[] = "/tmp/hmi-l1-trace-test-XXXXXX";
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    CHECK(close(descriptor) == 0);
    static const char *valid[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    write_fixture(path, valid, false, false);
    L1TraceInfo info;
    char error[128] = {0};
    uint64_t rows = 0;
    bool read_ok = l1_trace_read(path, count_row, &rows, &info, error, sizeof(error));
    if (!read_ok) fprintf(stderr, "fixture rejected: %s\n", error);
    CHECK(read_ok);
    CHECK(rows == 3 && info.count == 3 && !info.horizon_eligible);
    CHECK(info.cadence_ms == 1000 && info.sample_limited);
    expect_invalid(path, valid, true, false);
    expect_invalid(path, valid, false, true);

    static const char *repeated[3] = {
        "D,0,100,10,20,1000,100,200,1,0",
        "D,0,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, repeated, false, false);
    static const char *range[3] = {
        "D,0,10001,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, range, false, false);
    static const char *valid_two[3] = {
        "D,0,100,10,20,1000,100,200,2,0\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, valid_two, false, false);
    static const char *flags_four[3] = {
        "D,0,100,10,20,1000,100,200,1,4\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, flags_four, false, false);
    static const char *reset[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,900,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, reset, false, false);
    static const char *signed_value[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,-1,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    expect_invalid(path, signed_value, false, false);
    static const char *invalid_boundary[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,0,0,0,0,0,0,0,1\n",
        "D,2000,120,12,22,10,20,30,1,0\n"
    };
    write_fixture(path, invalid_boundary, false, false);
    CHECK(l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    static const char *valid_overrun[3] = {
        "D,0,100,10,20,1000,100,200,1,2\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    write_fixture(path, valid_overrun, false, false);
    CHECK(l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    static const char *invalid_overrun[3] = {
        "D,0,0,0,0,0,0,0,0,3\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2000,120,12,22,1200,120,220,1,0\n"
    };
    write_fixture(path, invalid_overrun, false, false);
    CHECK(l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));

    write_fixture_limit(path, valid, "duration", 2, false, false);
    CHECK(l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    static const char *duration_early[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,1999,120,12,22,1200,120,220,1,0\n"
    };
    write_fixture_limit(path, duration_early, "duration", 2, false, false);
    CHECK(!l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    static const char *duration_late[3] = {
        "D,0,100,10,20,1000,100,200,1,0\n",
        "D,1000,110,11,21,1100,110,210,1,0\n",
        "D,2001,120,12,22,1200,120,220,1,0\n"
    };
    write_fixture_limit(path, duration_late, "duration", 2, false, false);
    CHECK(l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    write_fixture_limit(path, valid, "samples", 4, false, false);
    CHECK(!l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));

    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fputs("HMI_L1_PSI_TRACE_V1\nschema=1\n", file) >= 0);
    CHECK(fclose(file) == 0);
    CHECK(!l1_trace_read(path, NULL, NULL, &info, error, sizeof(error)));
    CHECK(unlink(path) == 0);
    puts("PASS: V1 trace schema, checksum, streaming parser, and negative corpus");
    return 0;
}
