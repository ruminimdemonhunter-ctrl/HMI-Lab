#define _GNU_SOURCE
#include "l1_shadow.h"
#include "l1_trace.h"

#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum { PASS_REPLAY, PASS_FORMAT } PassMode;

typedef struct {
    PassMode mode;
    L1Core core;
    L1Core reference;
    L1Shadow shadow;
    uint64_t mismatches;
    uint64_t sink;
} BenchContext;

static uint64_t cpu_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static bool row_callback(const L1TraceRow *row, void *opaque) {
    BenchContext *context = opaque;
    if (context->mode == PASS_REPLAY) {
        L1Action action = l1_step_at(&context->core, row->observation,
                                     row->relative_ms);
        L1Action reference = l1_step_at(&context->reference, row->observation,
                                        row->relative_ms);
        (void)l1_shadow_step_at(&context->shadow, row->observation,
                                row->counters, row->relative_ms);
        if (action != reference ||
            memcmp(&context->core, &context->reference,
                   sizeof(context->core)) != 0)
            context->mismatches++;
        context->sink ^= (uint64_t)action + context->shadow.predictions_issued;
    } else {
        char line[256];
        int length = snprintf(line, sizeof(line),
            "time=%" PRIu64 " valid=%u cpu=%u memory=%u io=%u flags=%u\n",
            row->relative_ms, row->observation.valid ? 1U : 0U,
            row->observation.cpu_some_bp, row->observation.memory_full_bp,
            row->observation.io_full_bp, row->flags);
        if (length < 0 || (size_t)length >= sizeof(line)) return false;
        context->sink = l1_trace_hash_bytes(context->sink, line, (size_t)length);
    }
    return true;
}

static bool parse_repetitions(const char *text, unsigned *out) {
    if (text == NULL || *text == '\0') return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (digit > 20U || value > (20U - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    if (value == 0 || value > 20) return false;
    *out = value;
    return true;
}

static uint64_t timeval_us(struct timeval value) {
    return (uint64_t)value.tv_sec * UINT64_C(1000000) +
           (uint64_t)value.tv_usec;
}

static uint64_t timespec_ns(struct timespec value) {
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static bool run_cli(const char *path, uint64_t *cpu_ns_out,
                    uint64_t *wall_ns_out, long *rss_out) {
    struct timespec started, finished;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return false;
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) _exit(126);
        if (null_fd != STDOUT_FILENO) (void)close(null_fd);
        execl("./l1_inspect", "./l1_inspect", "trace", path,
              "--details", "0", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    struct rusage child_usage;
    if (wait4(child, &status, 0, &child_usage) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &finished) != 0)
        return false;
    *cpu_ns_out = (timeval_us(child_usage.ru_utime) +
                   timeval_us(child_usage.ru_stime)) * 1000U;
    *wall_ns_out = timespec_ns(finished) - timespec_ns(started);
    *rss_out = child_usage.ru_maxrss;
    return true;
}

static void sort_u64(uint64_t *values, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        uint64_t value = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static void sort_long(long *values, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        long value = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

int main(int argc, char **argv) {
    unsigned repetitions;
    if (argc != 3 || argv[1][0] != '/' ||
        !parse_repetitions(argv[2], &repetitions)) {
        fputs("usage: ./l1_inspect_bench ABSOLUTE_V1_TRACE REPETITIONS_1_TO_20\n",
              stderr);
        return EXIT_FAILURE;
    }
    uint64_t parser[20], core_shadow[20], formatting[20], replay_pass[20];
    uint64_t cli_cpu[20], cli_wall[20], sink = 0;
    long cli_rss[20];
    uint64_t rows = 0, expected_checksum = 0;
    char error[160];
    for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
        L1TraceInfo parsed, replayed, formatted;
        uint64_t started = cpu_ns();
        if (!l1_trace_read(argv[1], NULL, NULL, &parsed, error, sizeof(error))) {
            fprintf(stderr, "parse failed: %s\n", error);
            return EXIT_FAILURE;
        }
        parser[repetition] = cpu_ns() - started;

        BenchContext replay = {.mode = PASS_REPLAY};
        l1_init(&replay.core);
        l1_init(&replay.reference);
        l1_shadow_init(&replay.shadow, l1_default_forecast_thresholds());
        started = cpu_ns();
        if (!l1_trace_read(argv[1], row_callback, &replay, &replayed,
                           error, sizeof(error)) || replay.mismatches != 0) {
            fprintf(stderr, "replay failed: %s\n", error);
            return EXIT_FAILURE;
        }
        replay_pass[repetition] = cpu_ns() - started;
        core_shadow[repetition] = replay_pass[repetition] > parser[repetition] ?
            replay_pass[repetition] - parser[repetition] : 0;

        BenchContext format = {.mode = PASS_FORMAT,
                               .sink = UINT64_C(1469598103934665603)};
        started = cpu_ns();
        if (!l1_trace_read(argv[1], row_callback, &format, &formatted,
                           error, sizeof(error))) {
            fprintf(stderr, "format pass failed: %s\n", error);
            return EXIT_FAILURE;
        }
        uint64_t format_pass = cpu_ns() - started;
        formatting[repetition] = format_pass > parser[repetition] ?
            format_pass - parser[repetition] : 0;
        if (parsed.count != replayed.count || parsed.count != formatted.count ||
            parsed.checksum != replayed.checksum ||
            parsed.checksum != formatted.checksum) {
            fputs("pass identity mismatch\n", stderr);
            return EXIT_FAILURE;
        }
        rows = parsed.count;
        expected_checksum = parsed.checksum;
        sink ^= replay.sink ^ format.sink;
        if (!run_cli(argv[1], &cli_cpu[repetition], &cli_wall[repetition],
                     &cli_rss[repetition])) {
            fputs("CLI resource run failed\n", stderr);
            return EXIT_FAILURE;
        }
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return EXIT_FAILURE;
    sort_u64(parser, repetitions);
    sort_u64(core_shadow, repetitions);
    sort_u64(formatting, repetitions);
    sort_u64(replay_pass, repetitions);
    sort_u64(cli_cpu, repetitions);
    sort_u64(cli_wall, repetitions);
    sort_long(cli_rss, repetitions);
    unsigned median = repetitions / 2U;
#define PER_ROW(values, index) ((double)(values)[index] / (double)rows)
    printf("INSPECT_BENCH rows=%" PRIu64 " repetitions=%u"
           " parser_ns_per_row=%.3f(%.3f..%.3f)"
           " core_shadow_ns_per_row=%.3f(%.3f..%.3f)"
           " format_ns_per_row=%.3f(%.3f..%.3f)"
           " total_replay_pass_ns_per_row=%.3f(%.3f..%.3f)"
           " bench_rss_kib=%ld"
           " cli_cpu_ns_per_row=%.3f(%.3f..%.3f)"
           " cli_wall_ns_per_row=%.3f(%.3f..%.3f)"
           " cli_rss_kib=%ld(%ld..%ld)"
           " trace_checksum=%016" PRIx64 " sink=%016" PRIx64 "\n",
           rows, repetitions,
           PER_ROW(parser, median), PER_ROW(parser, 0),
           PER_ROW(parser, repetitions - 1U),
           PER_ROW(core_shadow, median), PER_ROW(core_shadow, 0),
           PER_ROW(core_shadow, repetitions - 1U),
           PER_ROW(formatting, median), PER_ROW(formatting, 0),
           PER_ROW(formatting, repetitions - 1U),
           PER_ROW(replay_pass, median), PER_ROW(replay_pass, 0),
           PER_ROW(replay_pass, repetitions - 1U), usage.ru_maxrss,
           PER_ROW(cli_cpu, median), PER_ROW(cli_cpu, 0),
           PER_ROW(cli_cpu, repetitions - 1U),
           PER_ROW(cli_wall, median), PER_ROW(cli_wall, 0),
           PER_ROW(cli_wall, repetitions - 1U),
           cli_rss[median], cli_rss[0], cli_rss[repetitions - 1U],
           expected_checksum, sink);
#undef PER_ROW
    return EXIT_SUCCESS;
}
