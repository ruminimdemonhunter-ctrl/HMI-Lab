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

enum { MAX_REPETITIONS = 5 };

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

static bool run_once(const char *path, uint64_t *cpu_ns, uint64_t *wall_ns,
                     long *rss_kib) {
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) return false;
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        int sink = open("/dev/null", O_WRONLY);
        if (sink < 0 || dup2(sink, STDOUT_FILENO) < 0 ||
            dup2(sink, STDERR_FILENO) < 0) _exit(126);
        if (sink > STDERR_FILENO) close(sink);
        execl("./l1_evidence", "./l1_evidence", "analyze", path,
              "--source", "synthetic-fixture", (char *)NULL);
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    struct rusage usage;
    if (wait4(child, &status, 0, &usage) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
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
        !decimal(argv[2], &repetitions)) {
        fputs("usage: ./l1_evidence_bench ABSOLUTE_TRACE 1..5 ROWS"
              " [instrumented]\n", stderr);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    errno = 0;
    uintmax_t rows = strtoumax(argv[3], &end, 10);
    if (errno || end == argv[3] || *end != '\0' || rows < 2 || rows > 86400)
        return EXIT_FAILURE;
    uint64_t cpu[MAX_REPETITIONS], wall[MAX_REPETITIONS];
    long rss[MAX_REPETITIONS];
    for (unsigned i = 0; i < repetitions; ++i)
        if (!run_once(argv[1], &cpu[i], &wall[i], &rss[i])) {
            fputs("evidence process failed\n", stderr);
            return EXIT_FAILURE;
        }
    sort_u64(cpu, repetitions);
    sort_u64(wall, repetitions);
    sort_long(rss, repetitions);
    unsigned middle = repetitions / 2U;
    printf("EVIDENCE_BENCH rows=%" PRIuMAX " repetitions=%u"
           " cpu_ns_per_row=%.3f(%.3f..%.3f)"
           " wall_ns_per_row=%.3f(%.3f..%.3f)"
           " rss_kib=%ld(%ld..%ld) rss_budget_kib=16384 budget=%s\n",
           rows, repetitions, (double)cpu[middle] / (double)rows,
           (double)cpu[0] / (double)rows,
           (double)cpu[repetitions - 1U] / (double)rows,
           (double)wall[middle] / (double)rows,
           (double)wall[0] / (double)rows,
           (double)wall[repetitions - 1U] / (double)rows,
           rss[middle], rss[0], rss[repetitions - 1U],
           instrumented ? "EXCLUDED_INSTRUMENTED" :
           (rss[repetitions - 1U] <= 16384 ? "PASS" : "FAIL"));
    return instrumented || rss[repetitions - 1U] <= 16384 ?
        EXIT_SUCCESS : EXIT_FAILURE;
}
