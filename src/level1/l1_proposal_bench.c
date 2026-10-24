#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MAX_REPETITIONS = 5, RSS_BUDGET_KIB = 8192 };

static bool decimal(const char *text, unsigned *out) {
    if (text == NULL || *text == '\0') return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (digit > MAX_REPETITIONS ||
            value > (MAX_REPETITIONS - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    if (value == 0) return false;
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

static bool run_once(const char *path, int expected_exit,
                     uint64_t *cpu_ns, uint64_t *wall_ns, long *rss_kib) {
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) return false;
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        int sink = open("/dev/null", O_WRONLY);
        if (sink < 0 || dup2(sink, STDOUT_FILENO) < 0 ||
            dup2(sink, STDERR_FILENO) < 0) _exit(126);
        if (sink > STDERR_FILENO) close(sink);
        execl("./l1_proposal_check", "./l1_proposal_check", path,
              "--current-generation", "0", (char *)NULL);
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    struct rusage usage;
    if (wait4(child, &status, 0, &usage) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != expected_exit ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0) return false;
    *cpu_ns = (timeval_us(usage.ru_utime) + timeval_us(usage.ru_stime)) * 1000U;
    *wall_ns = timespec_ns(end) - timespec_ns(begin);
    *rss_kib = usage.ru_maxrss;
    return true;
}

int main(int argc, char **argv) {
    unsigned repetitions;
    bool instrumented = argc == 5 && strcmp(argv[4], "instrumented") == 0;
    if ((argc != 4 && !instrumented) || argv[1][0] != '/' ||
        argv[2][0] != '/' || !decimal(argv[3], &repetitions)) {
        fputs("usage: ./l1_proposal_bench ABSOLUTE_VALID ABSOLUTE_NEGATIVE"
              " 1..5 [instrumented]\n", stderr);
        return EXIT_FAILURE;
    }
    uint64_t cpu[MAX_REPETITIONS], wall[MAX_REPETITIONS];
    long rss[MAX_REPETITIONS];
    for (unsigned i = 0; i < repetitions; ++i) {
        uint64_t valid_cpu, valid_wall, negative_cpu, negative_wall;
        long valid_rss, negative_rss;
        if (!run_once(argv[1], EXIT_SUCCESS, &valid_cpu, &valid_wall,
                      &valid_rss) ||
            !run_once(argv[2], EXIT_FAILURE, &negative_cpu, &negative_wall,
                      &negative_rss)) {
            fputs("proposal process failed\n", stderr);
            return EXIT_FAILURE;
        }
        cpu[i] = valid_cpu + negative_cpu;
        wall[i] = valid_wall + negative_wall;
        rss[i] = valid_rss > negative_rss ? valid_rss : negative_rss;
    }
    sort_u64(cpu, repetitions);
    sort_u64(wall, repetitions);
    sort_long(rss, repetitions);
    unsigned middle = repetitions / 2U;
    bool budget = rss[repetitions - 1U] <= RSS_BUDGET_KIB;
    printf("PROPOSAL_BENCH repetitions=%u pair=canonical-plus-max-negative"
           " cpu_ns=%" PRIu64 "(%" PRIu64 "..%" PRIu64 ")"
           " wall_ns=%" PRIu64 "(%" PRIu64 "..%" PRIu64 ")"
           " rss_kib=%ld(%ld..%ld) rss_budget_kib=%u budget=%s\n",
           repetitions, cpu[middle], cpu[0], cpu[repetitions - 1U],
           wall[middle], wall[0], wall[repetitions - 1U],
           rss[middle], rss[0], rss[repetitions - 1U], RSS_BUDGET_KIB,
           instrumented ? "EXCLUDED_INSTRUMENTED" :
           (budget ? "PASS" : "FAIL"));
    return instrumented || budget ? EXIT_SUCCESS : EXIT_FAILURE;
}
