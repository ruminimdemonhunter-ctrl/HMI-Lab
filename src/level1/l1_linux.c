#include "l1.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool l1_parse_psi_percent(const char *line, const char *category, uint16_t *bp) {
    if (line == NULL || category == NULL || bp == NULL) return false;
    size_t n = strlen(category);
    if (strncmp(line, category, n) != 0 || line[n] != ' ') return false;
    const char *p = strstr(line + n, " avg10=");
    if (p == NULL) return false;
    p += strlen(" avg10=");
    errno = 0;
    char *end = NULL;
    double percent = strtod(p, &end);
    if (errno || end == p || (*end != ' ' && *end != '\r' &&
                              *end != '\n' && *end != '\0')) return false;
    if (!(percent >= 0.0 && percent <= 100.0)) return false;
    *bp = (uint16_t)(percent * 100.0 + 0.5);
    return true;
}

bool l1_parse_psi_sample(const char *line, const char *category,
                         uint16_t *bp, uint64_t *total_us) {
    if (total_us == NULL || !l1_parse_psi_percent(line, category, bp))
        return false;
    const char *total = strstr(line, " total=");
    if (total == NULL) return false;
    total += strlen(" total=");
    if (*total < '0' || *total > '9') return false;
    errno = 0;
    char *total_end = NULL;
    unsigned long long parsed_total = strtoull(total, &total_end, 10);
    if (errno || total_end == total ||
        (*total_end != '\r' && *total_end != '\n' && *total_end != '\0'))
        return false;
    *total_us = (uint64_t)parsed_total;
    return true;
}

static bool read_psi(const char *path, const char *category, uint16_t *bp,
                     uint64_t *total_us) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (l1_parse_psi_sample(line, category, bp, total_us)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool l1_read_linux_observation(L1Observation *out) {
    L1PressureCounters counters;
    return l1_read_linux_sample(out, &counters);
}

bool l1_read_linux_sample(L1Observation *out, L1PressureCounters *counters) {
    if (out == NULL || counters == NULL) return false;
    *out = (L1Observation) {0};
    *counters = (L1PressureCounters) {0};
    out->valid = read_psi("/proc/pressure/cpu", "some", &out->cpu_some_bp,
                          &counters->cpu_some_total_us) &&
                 read_psi("/proc/pressure/memory", "full",
                          &out->memory_full_bp,
                          &counters->memory_full_total_us) &&
                 read_psi("/proc/pressure/io", "full", &out->io_full_bp,
                          &counters->io_full_total_us);
    counters->valid = out->valid;
    return out->valid;
}
