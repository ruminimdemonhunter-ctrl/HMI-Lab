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

static bool read_psi(const char *path, const char *category, uint16_t *bp) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (l1_parse_psi_percent(line, category, bp)) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool l1_read_linux_observation(L1Observation *out) {
    if (out == NULL) return false;
    *out = (L1Observation) {0};
    out->valid = read_psi("/proc/pressure/cpu", "some", &out->cpu_some_bp) &&
                 read_psi("/proc/pressure/memory", "full", &out->memory_full_bp) &&
                 read_psi("/proc/pressure/io", "full", &out->io_full_bp);
    return out->valid;
}
