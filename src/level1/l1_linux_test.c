#define _POSIX_C_SOURCE 200809L
#include "l1_linux.h"

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

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

static void write_oversized(const char *path) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fputs("full avg10=0.40 avg60=0.30 avg300=0.20 total=456 ", file) >= 0);
    for (unsigned i = 0; i < 1100; ++i) CHECK(fputc('x', file) == 'x');
    CHECK(fputc('\n', file) == '\n');
    CHECK(fclose(file) == 0);
}

static void check_equal(L1Observation left_observation,
                        L1PressureCounters left_counters,
                        L1Observation right_observation,
                        L1PressureCounters right_counters) {
    if (left_observation.cpu_some_bp != right_observation.cpu_some_bp ||
        left_observation.memory_full_bp != right_observation.memory_full_bp ||
        left_observation.io_full_bp != right_observation.io_full_bp) {
        fprintf(stderr, "PSI mismatch: baseline=%u/%u/%u candidate=%u/%u/%u\n",
                left_observation.cpu_some_bp, left_observation.memory_full_bp,
                left_observation.io_full_bp, right_observation.cpu_some_bp,
                right_observation.memory_full_bp, right_observation.io_full_bp);
    }
    CHECK(left_observation.valid == right_observation.valid);
    CHECK(left_observation.cpu_some_bp == right_observation.cpu_some_bp);
    CHECK(left_observation.memory_full_bp == right_observation.memory_full_bp);
    CHECK(left_observation.io_full_bp == right_observation.io_full_bp);
    CHECK(left_counters.valid == right_counters.valid);
    CHECK(left_counters.cpu_some_total_us == right_counters.cpu_some_total_us);
    CHECK(left_counters.memory_full_total_us ==
          right_counters.memory_full_total_us);
    CHECK(left_counters.io_full_total_us == right_counters.io_full_total_us);
}

static void compare_readers(L1LinuxPsiReader *reader, const char *cpu,
                            const char *memory, const char *io,
                            bool expected) {
    L1Observation baseline_observation, candidate_observation;
    L1PressureCounters baseline_counters, candidate_counters;
    bool baseline = l1_read_linux_sample_paths(cpu, memory, io,
                                               &baseline_observation,
                                               &baseline_counters);
    bool candidate = l1_linux_psi_reader_read(reader, &candidate_observation,
                                              &candidate_counters);
    CHECK(baseline == expected && candidate == expected);
    CHECK(baseline_observation.valid == expected);
    CHECK(candidate_observation.valid == expected);
    CHECK(baseline_counters.valid == expected);
    CHECK(candidate_counters.valid == expected);
    if (expected) {
        check_equal(baseline_observation, baseline_counters,
                    candidate_observation, candidate_counters);
    }
}

static void check_one_shot_only(L1LinuxPsiReader *reader, const char *cpu,
                                const char *memory, const char *io) {
    L1Observation observation;
    L1PressureCounters counters;
    CHECK(l1_read_linux_sample_paths(cpu, memory, io, &observation, &counters));
    CHECK(observation.valid && counters.valid);
    CHECK(observation.cpu_some_bp == 1234);
    CHECK(observation.memory_full_bp == 40);
    CHECK(observation.io_full_bp == 150);
    CHECK(counters.cpu_some_total_us == 123456);
    CHECK(counters.memory_full_total_us == 456);
    CHECK(counters.io_full_total_us == 789);

    CHECK(!l1_linux_psi_reader_read(reader, &observation, &counters));
    CHECK(!observation.valid && !counters.valid);
}

int main(void) {
    char directory[] = "/tmp/hmi-l1-linux-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char cpu[256], memory[256], io[256], missing[256];
    CHECK(snprintf(cpu, sizeof(cpu), "%s/cpu", directory) > 0);
    CHECK(snprintf(memory, sizeof(memory), "%s/memory", directory) > 0);
    CHECK(snprintf(io, sizeof(io), "%s/io", directory) > 0);
    CHECK(snprintf(missing, sizeof(missing), "%s/missing", directory) > 0);

    write_text(cpu, "some avg10=12.34 avg60=1.00 avg300=2.00 total=123456\n"
                    "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    write_text(memory, "some avg10=4.00 avg60=3.00 avg300=2.00 total=900\n"
                       "full avg10=0.40 avg60=0.30 avg300=0.20 total=456\n");
    write_text(io, "some avg10=8.00 avg60=7.00 avg300=6.00 total=800\n"
                   "full avg10=1.50 avg60=1.00 avg300=0.50 total=789\n");

    L1LinuxPsiReader reader;
    CHECK(l1_linux_psi_reader_open_paths(&reader, cpu, memory, io));
    compare_readers(&reader, cpu, memory, io, true);

    /* V0.7's fgets baseline accepts a syntactically complete final line even
       without a newline. The bounded pread reader requires complete procfs
       records and therefore rejects this non-procfs fixture. */
    write_text(cpu, "some avg10=12.34 avg60=1.00 avg300=2.00 total=123456");
    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20 total=456");
    write_text(io, "full avg10=1.50 avg60=1.00 avg300=0.50 total=789");
    check_one_shot_only(&reader, cpu, memory, io);

    write_text(cpu, "some avg10=12.34 avg60=1.00 avg300=2.00 total=123456\n");
    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20 total=456\n");
    write_text(io, "full avg10=1.50 avg60=1.00 avg300=0.50 total=789\n");
    compare_readers(&reader, cpu, memory, io, true);

    write_text(cpu, "some avg10=100.00 avg60=0.00 avg300=0.00 total=18446744073709551615\n");
    write_text(memory, "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    write_text(io, "full avg10=99.99 avg60=0.00 avg300=0.00 total=42\n");
    compare_readers(&reader, cpu, memory, io, true);

    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20\n");
    compare_readers(&reader, cpu, memory, io, false);
    write_text(memory, "");
    compare_readers(&reader, cpu, memory, io, false);
    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20 total=\n");
    compare_readers(&reader, cpu, memory, io, false);
    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20 total=456x\n");
    compare_readers(&reader, cpu, memory, io, false);
    write_text(memory, "full avg10=0.40 avg60=0.30 avg300=0.20\n"
                       "some avg10=2.00 avg60=1.00 avg300=0.50 total=456\n");
    compare_readers(&reader, cpu, memory, io, false);
    write_oversized(memory);
    compare_readers(&reader, cpu, memory, io, false);

    CHECK(close(reader.memory_fd) == 0);
    reader.memory_fd = -1;
    L1Observation failed_observation;
    L1PressureCounters failed_counters;
    CHECK(!l1_linux_psi_reader_read(&reader, &failed_observation,
                                    &failed_counters));

    l1_linux_psi_reader_close(&reader);
    CHECK(reader.cpu_fd == -1 && reader.memory_fd == -1 && reader.io_fd == -1);
    CHECK(!l1_linux_psi_reader_open_paths(&reader, cpu, missing, io));
    CHECK(reader.cpu_fd == -1 && reader.memory_fd == -1 && reader.io_fd == -1);

    CHECK(unlink(cpu) == 0);
    CHECK(unlink(memory) == 0);
    CHECK(unlink(io) == 0);
    CHECK(rmdir(directory) == 0);
    puts("PASS: V0.7 EOF compatibility, strict records, and supported PSI equivalence");
    return 0;
}
