#include "l1.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *err, size_t size, const char *message) {
    if (err != NULL && size) snprintf(err, size, "%s", message);
}

bool l1_read_policy(const char *path, L1Policy *out, char *err, size_t size) {
    if (path == NULL || out == NULL) return false;
    FILE *f = fopen(path, "r");
    if (!f) {
        set_error(err, size, "cannot open proposal file");
        return false;
    }
    /* The file is treated as untrusted configuration, never as executable code.
       Reject unknown, duplicated, malformed and missing keys. */
    enum { FIELD_COUNT = 6 };
    const char *keys[FIELD_COUNT] = {
        "schema_version", "generation", "cpu_trigger_bp",
        "memory_trigger_bp", "io_trigger_bp", "confirmation_samples"
    };
    unsigned long values[FIELD_COUNT] = {0};
    bool seen[FIELD_COUNT] = {false};
    char line[160];
    bool valid = true;
    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) == sizeof(line) - 1 && !strchr(line, '\n')) { valid = false; break; }
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '#' || *start == '\n' || *start == '\0') continue;
        char *equal = strchr(start, '=');
        if (!equal) { valid = false; break; }
        *equal = '\0';
        char *keyend = equal;
        while (keyend > start && (keyend[-1] == ' ' || keyend[-1] == '\t')) {
            --keyend;
            *keyend = '\0';
        }
        int field = -1;
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (strcmp(start, keys[i]) == 0) { field = i; break; }
        }
        if (field < 0 || seen[field]) { valid = false; break; }
        char *num = equal + 1;
        while (*num == ' ' || *num == '\t') num++;
        if (*num < '0' || *num > '9') { valid = false; break; }
        errno = 0;
        char *end;
        unsigned long parsed = strtoul(num, &end, 10);
        if (errno || end == num) { valid = false; break; }
        while (*end == ' ' || *end == '\t' || *end == '\r') end++;
        if (*end != '\n' && *end != '\0') { valid = false; break; }
        if (parsed > UINT32_MAX) { valid = false; break; }
        seen[field] = true;
        values[field] = parsed;
    }
    if (ferror(f)) valid = false;
    fclose(f);
    for (int i = 0; i < FIELD_COUNT; i++) if (!seen[i]) valid = false;
    if (!valid || values[2] > UINT16_MAX || values[3] > UINT16_MAX ||
        values[4] > UINT16_MAX || values[5] > UINT8_MAX) {
        set_error(err, size, "malformed, missing, duplicate or out-of-range field");
        return false;
    }
    L1Policy proposal = {
        .schema_version = (uint32_t)values[0],
        .generation = (uint32_t)values[1],
        .cpu_trigger_bp = (uint16_t)values[2],
        .memory_trigger_bp = (uint16_t)values[3],
        .io_trigger_bp = (uint16_t)values[4],
        .confirmation_samples = (uint8_t)values[5]
    };
    if (!l1_policy_valid(&proposal)) {
        set_error(err, size, "policy violates immutable safety envelope");
        return false;
    }
    *out = proposal;
    return true;
}
