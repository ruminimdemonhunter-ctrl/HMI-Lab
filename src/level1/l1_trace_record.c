#define _XOPEN_SOURCE 700
#include "l1_linux.h"
#include "l1_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void stop_recording(int signal_number) { (void)signal_number; interrupted = 1; }

static bool bounded(const char *text, uint32_t low, uint32_t high, uint32_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (unsigned)(*p - '0');
        if (value > high) return false;
    }
    if (value < low) return false;
    *out = (uint32_t)value;
    return true;
}

static uint64_t milliseconds(struct timespec value) {
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static uint64_t timeval_nanoseconds(struct timeval value) {
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_usec * UINT64_C(1000);
}

static uint64_t usage_nanoseconds(struct rusage value) {
    return timeval_nanoseconds(value.ru_utime) +
        timeval_nanoseconds(value.ru_stime);
}

static uint64_t elapsed_nanoseconds(struct timespec start, struct timespec end) {
    uint64_t start_ns = (uint64_t)start.tv_sec * UINT64_C(1000000000) +
        (uint64_t)start.tv_nsec;
    uint64_t end_ns = (uint64_t)end.tv_sec * UINT64_C(1000000000) +
        (uint64_t)end.tv_nsec;
    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

static bool safe_parent(const char *path) {
    if (path == NULL || path[0] != '/') return false;
    char copy[4096], resolved[4096];
    if (strlen(path) >= sizeof(copy)) return false;
    strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) return false;
    if (slash == copy) slash[1] = '\0'; else *slash = '\0';
    if (realpath(copy, resolved) == NULL) return false;
    static const char source[] = "/home/lab/hmi/src";
    size_t n = strlen(source);
    return strncmp(resolved, source, n) != 0 ||
        (resolved[n] != '\0' && resolved[n] != '/');
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

int main(int argc, char **argv) {
    struct rusage usage_process_start;
    struct timespec wall_process_start;
    if (getrusage(RUSAGE_SELF, &usage_process_start) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_process_start) != 0) {
        fputs("recorder timing initialization failed\n", stderr);
        return 2;
    }
    const char *output = NULL;
    uint32_t samples = 0, duration = 0, cadence = 1000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc &&
                 bounded(argv[++i], 2, L1_TRACE_MAX_SAMPLES, &samples)) {}
        else if (!strcmp(argv[i], "--duration-sec") && i + 1 < argc &&
                 bounded(argv[++i], 2, 86400, &duration)) {}
        else if (!strcmp(argv[i], "--cadence-ms") && i + 1 < argc &&
                 bounded(argv[++i], L1_TRACE_MIN_CADENCE_MS,
                         L1_TRACE_MAX_CADENCE_MS, &cadence)) {}
        else {
            fputs("Usage: l1_trace_record --output ABSOLUTE_PATH "
                  "(--samples 2..86400 | --duration-sec 2..86400) "
                  "[--cadence-ms 1000..60000]\n", stderr);
            return 2;
        }
    }
    if (output == NULL || (samples == 0) == (duration == 0) ||
        !safe_parent(output) || access(output, F_OK) == 0) {
        fputs("refusing unsafe, existing, or incompletely bounded output\n", stderr);
        return 2;
    }
    if (duration && !l1_trace_duration_fits(duration, cadence)) {
        fputs("duration and cadence require more than 86400 trace samples\n",
              stderr);
        return 2;
    }
    char temporary[4096];
    int temporary_size = snprintf(temporary, sizeof(temporary), "%s.partial", output);
    if (temporary_size <= 0 || (size_t)temporary_size >= sizeof(temporary)) return 2;
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) { perror("open temporary trace"); return 2; }
    FILE *file = fdopen(descriptor, "wb");
    if (file == NULL) { close(descriptor); unlink(temporary); return 2; }
    L1LinuxPsiReader reader;
    if (!l1_linux_psi_reader_open(&reader)) {
        (void)fclose(file);
        (void)unlink(temporary);
        fputs("opening Linux PSI sources failed\n", stderr);
        return 2;
    }
    (void)signal(SIGINT, stop_recording);
    (void)signal(SIGTERM, stop_recording);
    uint64_t hash = UINT64_C(1469598103934665603), bytes = 0;
    char line[512];
    const char *limit_name = samples ? "samples" : "duration";
    uint32_t limit = samples ? samples : duration;
    const char *headers[] = {
        "HMI_L1_PSI_TRACE_V1\n", "schema=1\n",
        "units=relative_ms,basis_points,microseconds\n"
    };
    bool ok = true;
    for (size_t i = 0; i < 3; ++i)
        ok = ok && write_hashed(file, &hash, &bytes, headers[i]);
    (void)snprintf(line, sizeof(line), "cadence_ms=%u\n", cadence);
    ok = ok && write_hashed(file, &hash, &bytes, line);
    (void)snprintf(line, sizeof(line), "limit=%s\n", limit_name);
    ok = ok && write_hashed(file, &hash, &bytes, line);
    (void)snprintf(line, sizeof(line), "limit_value=%u\n", limit);
    ok = ok && write_hashed(file, &hash, &bytes, line);
    ok = ok && write_hashed(file, &hash, &bytes,
        "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
        "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n");
    struct rusage usage_loop_start, usage_loop_end;
    if (getrusage(RUSAGE_SELF, &usage_loop_start) != 0) ok = false;
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) ok = false;
    uint64_t count = 0;
    while (ok && !interrupted) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { ok = false; break; }
        uint64_t relative = milliseconds(now) - milliseconds(start);
        L1Observation observation;
        L1PressureCounters counters;
        bool valid = l1_linux_psi_reader_read(&reader, &observation, &counters);
        uint8_t flags = valid ? 0U : 1U;
        if (!valid) {
            observation = (L1Observation){0, 0, 0, false};
            counters = (L1PressureCounters){0, 0, 0, false};
        }
        int written = snprintf(line, sizeof(line),
            "D,%" PRIu64 ",%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u\n",
            relative, observation.cpu_some_bp, observation.memory_full_bp,
            observation.io_full_bp, counters.cpu_some_total_us,
            counters.memory_full_total_us, counters.io_full_total_us,
            valid ? 1U : 0U, flags);
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !write_hashed(file, &hash, &bytes, line)) { ok = false; break; }
        count++;
        if (samples && count >= samples) break;
        if (duration && count >= 2 && relative >= (uint64_t)duration * 1000U)
            break;
        uint64_t target_ms = milliseconds(start) +
            l1_trace_next_offset_ms(count, cadence, duration);
        struct timespec target = {(time_t)(target_ms / 1000U),
                                  (long)(target_ms % 1000U) * 1000000L};
        int sleep_result;
        do {
            sleep_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                           &target, NULL);
        } while (sleep_result == EINTR && !interrupted);
        if (sleep_result != 0 && !interrupted) ok = false;
    }
    if (getrusage(RUSAGE_SELF, &usage_loop_end) != 0) ok = false;
    l1_linux_psi_reader_close(&reader);
    if (count < 2) ok = false;
    int footer = snprintf(line, sizeof(line), "END,count=%" PRIu64
                          ",fnv1a64=%016" PRIx64 "\n", count, hash);
    bool published = ok && !interrupted && footer > 0 &&
        (size_t)footer < sizeof(line) &&
        bytes + (uint64_t)footer <= L1_TRACE_MAX_BYTES &&
        fputs(line, file) >= 0 && fflush(file) == 0 && fsync(descriptor) == 0;
    if (fclose(file) != 0) published = false;
    if (published && link(temporary, output) != 0) published = false;
    if (published && unlink(temporary) != 0) published = false;
    if (!published) {
        (void)unlink(temporary);
        fputs(interrupted ? "recording interrupted; partial removed\n" :
                            "recording failed; partial removed\n", stderr);
        return interrupted ? 130 : 2;
    }
    struct rusage usage_end;
    struct timespec wall_end;
    if (getrusage(RUSAGE_SELF, &usage_end) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &wall_end) != 0) {
        fputs("recorder final timing failed\n", stderr);
        return 2;
    }
    uint64_t process_start_ns = usage_nanoseconds(usage_process_start);
    uint64_t loop_start_ns = usage_nanoseconds(usage_loop_start);
    uint64_t loop_end_ns = usage_nanoseconds(usage_loop_end);
    uint64_t end_ns = usage_nanoseconds(usage_end);
    uint64_t setup_cpu_ns = loop_start_ns >= process_start_ns ?
        loop_start_ns - process_start_ns : 0;
    uint64_t loop_cpu_ns = loop_end_ns >= loop_start_ns ?
        loop_end_ns - loop_start_ns : 0;
    uint64_t publication_cpu_ns = end_ns >= loop_end_ns ?
        end_ns - loop_end_ns : 0;
    uint64_t total_cpu_ns = end_ns >= process_start_ns ?
        end_ns - process_start_ns : 0;
    uint64_t capture_cpu_ns = loop_cpu_ns + publication_cpu_ns;
    uint64_t wall_ns = elapsed_nanoseconds(wall_process_start, wall_end);
    printf("TRACE_RECORDED,samples=%" PRIu64 ",bytes=%" PRIu64
           ",checksum=%016" PRIx64 ",cpu_ns_per_sample=%.3f"
           ",total_cpu_ns_per_sample=%.3f,setup_cpu_ns=%" PRIu64
           ",loop_cpu_ns=%" PRIu64 ",publication_cpu_ns=%" PRIu64
           ",total_cpu_ns=%" PRIu64 ",wall_ns=%" PRIu64
           ",rss_kib=%ld\n",
           count, bytes + (uint64_t)footer, hash,
           count ? (double)capture_cpu_ns / (double)count : 0.0,
           count ? (double)total_cpu_ns / (double)count : 0.0,
           setup_cpu_ns, loop_cpu_ns, publication_cpu_ns, total_cpu_ns,
           wall_ns, usage_end.ru_maxrss);
    return 0;
}
