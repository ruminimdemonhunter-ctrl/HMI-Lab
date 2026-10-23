#define _POSIX_C_SOURCE 200809L
#include "l1_linux.h"
#include "l1_trace.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef enum { BENCH_BASELINE, BENCH_CANDIDATE } BenchMode;

static uint64_t usage_ns(struct rusage usage) {
    return (uint64_t)usage.ru_utime.tv_sec * UINT64_C(1000000000) +
        (uint64_t)usage.ru_utime.tv_usec * 1000U +
        (uint64_t)usage.ru_stime.tv_sec * UINT64_C(1000000000) +
        (uint64_t)usage.ru_stime.tv_usec * 1000U;
}

static uint64_t wall_ns(struct timespec start, struct timespec end) {
    uint64_t first = (uint64_t)start.tv_sec * UINT64_C(1000000000) +
        (uint64_t)start.tv_nsec;
    uint64_t last = (uint64_t)end.tv_sec * UINT64_C(1000000000) +
        (uint64_t)end.tv_nsec;
    return last >= first ? last - first : 0;
}

static bool parse_count(const char *text, uint32_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (unsigned)(*p - '0');
        if (value > L1_TRACE_MAX_SAMPLES) return false;
    }
    if (value < 2) return false;
    *out = (uint32_t)value;
    return true;
}

static bool write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fputs(text, file) >= 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool write_hashed(FILE *file, uint64_t *hash, uint64_t *bytes,
                         const char *line) {
    size_t size = strlen(line);
    if (*bytes + size + 64U > L1_TRACE_MAX_BYTES || fputs(line, file) < 0)
        return false;
    *hash = l1_trace_hash_bytes(*hash, line, size);
    *bytes += size;
    return true;
}

static bool read_mode(BenchMode mode, L1LinuxPsiReader *reader,
                      const char *cpu, const char *memory, const char *io,
                      L1Observation *observation,
                      L1PressureCounters *counters) {
    return mode == BENCH_BASELINE ?
        l1_read_linux_sample_paths(cpu, memory, io, observation, counters) :
        l1_linux_psi_reader_read(reader, observation, counters);
}

static bool open_mode(BenchMode mode, L1LinuxPsiReader *reader,
                      const char *cpu, const char *memory, const char *io) {
    *reader = (L1LinuxPsiReader){-1, -1, -1};
    return mode == BENCH_BASELINE ||
        l1_linux_psi_reader_open_paths(reader, cpu, memory, io);
}

static void cleanup(const char *output, const char *partial, const char *cpu,
                    const char *memory, const char *io, const char *directory) {
    (void)unlink(output);
    (void)unlink(partial);
    (void)unlink(cpu);
    (void)unlink(memory);
    (void)unlink(io);
    (void)rmdir(directory);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: l1_recorder_bench baseline|candidate SAMPLES\n", stderr);
        return 2;
    }
    BenchMode mode;
    if (strcmp(argv[1], "baseline") == 0) mode = BENCH_BASELINE;
    else if (strcmp(argv[1], "candidate") == 0) mode = BENCH_CANDIDATE;
    else {
        fputs("mode must be baseline or candidate\n", stderr);
        return 2;
    }
    uint32_t samples;
    if (!parse_count(argv[2], &samples)) {
        fputs("samples must be 2..86400\n", stderr);
        return 2;
    }

    char directory[] = "/tmp/hmi-l1-recorder-bench-XXXXXX";
    if (mkdtemp(directory) == NULL) return 2;
    char cpu[256], memory[256], io[256], output[256], partial[256];
    if (snprintf(cpu, sizeof(cpu), "%s/cpu", directory) <= 0 ||
        snprintf(memory, sizeof(memory), "%s/memory", directory) <= 0 ||
        snprintf(io, sizeof(io), "%s/io", directory) <= 0 ||
        snprintf(output, sizeof(output), "%s/result.l1trace", directory) <= 0 ||
        snprintf(partial, sizeof(partial), "%s.partial", output) <= 0) {
        cleanup(output, partial, cpu, memory, io, directory);
        return 2;
    }
    bool ok = write_text(cpu,
        "some avg10=12.34 avg60=1.00 avg300=2.00 total=123456\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n") &&
        write_text(memory,
        "some avg10=4.00 avg60=3.00 avg300=2.00 total=900\n"
        "full avg10=0.40 avg60=0.30 avg300=0.20 total=456\n") &&
        write_text(io,
        "some avg10=8.00 avg60=7.00 avg300=6.00 total=800\n"
        "full avg10=1.50 avg60=1.00 avg300=0.50 total=789\n");
    struct rusage process_start, loop_start, loop_end, process_end;
    struct timespec wall_start, wall_end;
    if (!ok || getrusage(RUSAGE_SELF, &process_start) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_start) != 0) {
        cleanup(output, partial, cpu, memory, io, directory);
        return 2;
    }

    int descriptor = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *file = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
    L1LinuxPsiReader reader = {-1, -1, -1};
    if (file == NULL || !open_mode(mode, &reader, cpu, memory, io)) ok = false;
    uint64_t hash = UINT64_C(1469598103934665603), bytes = 0;
    static const char *headers[] = {
        "HMI_L1_PSI_TRACE_V1\n", "schema=1\n",
        "units=relative_ms,basis_points,microseconds\n",
        "cadence_ms=1000\n", "limit=samples\n"
    };
    for (size_t i = 0; ok && i < sizeof(headers) / sizeof(headers[0]); ++i)
        ok = write_hashed(file, &hash, &bytes, headers[i]);
    char line[L1_TRACE_MAX_LINE];
    int length = snprintf(line, sizeof(line), "limit_value=%u\n", samples);
    ok = ok && length > 0 && (size_t)length < sizeof(line) &&
        write_hashed(file, &hash, &bytes, line) &&
        write_hashed(file, &hash, &bytes,
        "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
        "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n");
    if (getrusage(RUSAGE_SELF, &loop_start) != 0) ok = false;
    for (uint32_t i = 0; ok && i < samples; ++i) {
        L1Observation observation;
        L1PressureCounters counters;
        ok = read_mode(mode, &reader, cpu, memory, io, &observation, &counters);
        length = snprintf(line, sizeof(line),
            "D,%" PRIu64 ",%u,%u,%u,%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",1,0\n", (uint64_t)i * 1000U,
            observation.cpu_some_bp, observation.memory_full_bp,
            observation.io_full_bp, counters.cpu_some_total_us,
            counters.memory_full_total_us, counters.io_full_total_us);
        ok = ok && length > 0 && (size_t)length < sizeof(line) &&
            write_hashed(file, &hash, &bytes, line);
    }
    if (getrusage(RUSAGE_SELF, &loop_end) != 0) ok = false;
    l1_linux_psi_reader_close(&reader);
    length = snprintf(line, sizeof(line), "END,count=%u,fnv1a64=%016" PRIx64
                      "\n", samples, hash);
    ok = ok && length > 0 && (size_t)length < sizeof(line) &&
        bytes + (uint64_t)length <= L1_TRACE_MAX_BYTES &&
        fputs(line, file) >= 0 && fflush(file) == 0 && fsync(descriptor) == 0;
    if (file != NULL && fclose(file) != 0) ok = false;
    if (ok && link(partial, output) != 0) ok = false;
    if (ok && unlink(partial) != 0) ok = false;
    if (getrusage(RUSAGE_SELF, &process_end) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_end) != 0) ok = false;

    L1TraceInfo info;
    char error[128] = {0};
    if (ok) ok = l1_trace_read(output, NULL, NULL, &info, error, sizeof(error)) &&
        info.count == samples && info.checksum == hash;

    L1LinuxPsiReader hot_reader = {-1, -1, -1};
    L1Observation hot_observation;
    L1PressureCounters hot_counters;
    struct rusage hot_start, hot_end;
    if (ok) ok = open_mode(mode, &hot_reader, cpu, memory, io) &&
        read_mode(mode, &hot_reader, cpu, memory, io, &hot_observation,
                  &hot_counters) && getrusage(RUSAGE_SELF, &hot_start) == 0;
    for (uint32_t i = 0; ok && i < samples; ++i)
        ok = read_mode(mode, &hot_reader, cpu, memory, io, &hot_observation,
                       &hot_counters);
    if (ok && getrusage(RUSAGE_SELF, &hot_end) != 0) ok = false;
    l1_linux_psi_reader_close(&hot_reader);

    if (!ok) {
        fprintf(stderr, "recorder benchmark failed: %s\n", error);
        cleanup(output, partial, cpu, memory, io, directory);
        return 1;
    }
    uint64_t start_cpu = usage_ns(process_start);
    uint64_t loop_start_cpu = usage_ns(loop_start);
    uint64_t loop_end_cpu = usage_ns(loop_end);
    uint64_t end_cpu = usage_ns(process_end);
    uint64_t setup_cpu = loop_start_cpu - start_cpu;
    uint64_t loop_cpu = loop_end_cpu - loop_start_cpu;
    uint64_t publication_cpu = end_cpu - loop_end_cpu;
    uint64_t total_cpu = end_cpu - start_cpu;
    uint64_t hot_cpu = usage_ns(hot_end) - usage_ns(hot_start);
    printf("RECORDER_BENCH,mode=%s,samples=%u,valid=%u,checksum=%016" PRIx64
           ",setup_cpu_ns=%" PRIu64 ",loop_cpu_ns=%" PRIu64
           ",publication_cpu_ns=%" PRIu64 ",total_cpu_ns=%" PRIu64
           ",total_cpu_ns_per_sample=%.3f,hot_cpu_ns=%" PRIu64
           ",hot_cpu_ns_per_sample=%.3f,wall_ns=%" PRIu64
           ",rss_kib=%ld,bytes=%" PRIu64 "\n",
           mode == BENCH_BASELINE ? "baseline" : "candidate", samples,
           samples, hash, setup_cpu, loop_cpu, publication_cpu, total_cpu,
           (double)total_cpu / samples, hot_cpu, (double)hot_cpu / samples,
           wall_ns(wall_start, wall_end), process_end.ru_maxrss, info.bytes);
    cleanup(output, partial, cpu, memory, io, directory);
    return 0;
}
