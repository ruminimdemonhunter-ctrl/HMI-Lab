#define _POSIX_C_SOURCE 200809L
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
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &finished) != 0)
        return false;
    struct rusage children;
    if (getrusage(RUSAGE_CHILDREN, &children) != 0) return false;
    *cpu_ns_out = (timeval_us(children.ru_utime) +
                   timeval_us(children.ru_stime)) * 1000U;
    *wall_ns_out = timespec_ns(finished) - timespec_ns(started);
    *rss_out = children.ru_maxrss;
    return true;
}

int main(int argc, char **argv) {
    unsigned repetitions;
    if (argc != 3 || argv[1][0] != '/' ||
        !parse_repetitions(argv[2], &repetitions)) {
        fputs("usage: ./l1_inspect_bench ABSOLUTE_V1_TRACE REPETITIONS_1_TO_20\n",
              stderr);
        return EXIT_FAILURE;
    }
    uint64_t parse_ns = 0, replay_ns = 0, format_ns = 0, sink = 0;
    uint64_t rows = 0, expected_checksum = 0;
    char error[160];
    for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
        L1TraceInfo parsed, replayed, formatted;
        uint64_t started = cpu_ns();
        if (!l1_trace_read(argv[1], NULL, NULL, &parsed, error, sizeof(error))) {
            fprintf(stderr, "parse failed: %s\n", error);
            return EXIT_FAILURE;
        }
        parse_ns += cpu_ns() - started;

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
        replay_ns += cpu_ns() - started;

        BenchContext format = {.mode = PASS_FORMAT,
                               .sink = UINT64_C(1469598103934665603)};
        started = cpu_ns();
        if (!l1_trace_read(argv[1], row_callback, &format, &formatted,
                           error, sizeof(error))) {
            fprintf(stderr, "format pass failed: %s\n", error);
            return EXIT_FAILURE;
        }
        format_ns += cpu_ns() - started;
        if (parsed.count != replayed.count || parsed.count != formatted.count ||
            parsed.checksum != replayed.checksum ||
            parsed.checksum != formatted.checksum) {
            fputs("pass identity mismatch\n", stderr);
            return EXIT_FAILURE;
        }
        rows = parsed.count;
        expected_checksum = parsed.checksum;
        sink ^= replay.sink ^ format.sink;
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return EXIT_FAILURE;
    uint64_t cli_cpu_ns, cli_wall_ns;
    long cli_rss_kib;
    if (!run_cli(argv[1], &cli_cpu_ns, &cli_wall_ns, &cli_rss_kib)) {
        fputs("CLI resource run failed\n", stderr);
        return EXIT_FAILURE;
    }
    uint64_t denominator = rows * repetitions;
    uint64_t replay_delta = replay_ns > parse_ns ? replay_ns - parse_ns : 0;
    uint64_t format_delta = format_ns > parse_ns ? format_ns - parse_ns : 0;
    printf("INSPECT_BENCH rows=%" PRIu64 " repetitions=%u"
           " parser_ns_per_row=%.3f core_shadow_ns_per_row=%.3f"
           " format_ns_per_row=%.3f total_replay_pass_ns_per_row=%.3f"
           " bench_rss_kib=%ld cli_cpu_ns_per_row=%.3f"
           " cli_wall_ns_per_row=%.3f cli_rss_kib=%ld"
           " trace_checksum=%016" PRIx64 " sink=%016" PRIx64 "\n",
           rows, repetitions, (double)parse_ns / denominator,
           (double)replay_delta / denominator,
           (double)format_delta / denominator,
           (double)replay_ns / denominator, usage.ru_maxrss,
           (double)cli_cpu_ns / rows, (double)cli_wall_ns / rows, cli_rss_kib,
           expected_checksum, sink);
    return EXIT_SUCCESS;
}
