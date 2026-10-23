#define _POSIX_C_SOURCE 200809L
#include "l1_trace.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void set_error(char *error, size_t size, const char *format, ...) {
    if (error == NULL || size == 0) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

uint64_t l1_trace_hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool l1_trace_duration_fits(uint32_t duration_sec, uint32_t cadence_ms) {
    uint64_t duration_ms = (uint64_t)duration_sec * 1000U;
    uint64_t last_available_offset =
        (uint64_t)(L1_TRACE_MAX_SAMPLES - 1) * cadence_ms;
    return duration_ms <= last_available_offset;
}

uint64_t l1_trace_next_offset_ms(uint64_t samples_written,
                                 uint32_t cadence_ms, uint32_t duration_sec) {
    uint64_t offset = samples_written * cadence_ms;
    uint64_t deadline = (uint64_t)duration_sec * 1000U;
    return duration_sec != 0 && offset > deadline ? deadline : offset;
}

static bool clean_line(const char *line, size_t length) {
    if (length == 0 || length > L1_TRACE_MAX_LINE || line[length - 1] != '\n')
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)line[i];
        if (value == '\r' || (value != '\n' && (value < 32 || value > 126)))
            return false;
    }
    return true;
}

static bool decimal_u64(const char *text, uint64_t maximum, uint64_t *out) {
    if (text == NULL || *text == '\0') return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static bool header_value(const char *line, const char *prefix, uint64_t maximum,
                         uint64_t *out) {
    size_t prefix_size = strlen(prefix), line_size = strlen(line);
    if (line_size <= prefix_size + 1U || line[line_size - 1] != '\n' ||
        strncmp(line, prefix, prefix_size) != 0) return false;
    char value[64];
    size_t value_size = line_size - prefix_size - 1U;
    if (value_size >= sizeof(value)) return false;
    memcpy(value, line + prefix_size, value_size);
    value[value_size] = '\0';
    return decimal_u64(value, maximum, out);
}

static bool split_fields(char *line, char **fields, size_t expected) {
    size_t count = 0;
    char *start = line;
    for (char *p = line;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (count >= expected || p == start) return false;
            char separator = *p;
            *p = '\0';
            fields[count++] = start;
            if (separator == '\0') break;
            start = p + 1;
        }
    }
    return count == expected;
}

static bool parse_row(const char *source, L1TraceRow *row) {
    char copy[L1_TRACE_MAX_LINE + 1];
    size_t size = strlen(source);
    if (size < 3 || size > L1_TRACE_MAX_LINE || source[size - 1] != '\n')
        return false;
    memcpy(copy, source, size - 1U);
    copy[size - 1U] = '\0';
    char *field[10];
    if (!split_fields(copy, field, 10) || strcmp(field[0], "D") != 0)
        return false;
    uint64_t value[9];
    const uint64_t maximum[9] = {
        UINT64_MAX, 10000, 10000, 10000, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, 1, 3
    };
    for (size_t i = 0; i < 9; ++i)
        if (!decimal_u64(field[i + 1], maximum[i], &value[i])) return false;
    row->relative_ms = value[0];
    row->observation = (L1Observation){
        (uint16_t)value[1], (uint16_t)value[2], (uint16_t)value[3], value[7] != 0
    };
    row->counters = (L1PressureCounters){
        value[4], value[5], value[6], value[7] != 0
    };
    row->flags = (uint8_t)value[8];
    if (!row->observation.valid) {
        if ((row->flags & 1U) == 0 || value[1] || value[2] || value[3] ||
            value[4] || value[5] || value[6]) return false;
    } else if (row->flags & 1U) {
        return false;
    }
    return true;
}

static bool parse_footer(const char *line, uint64_t *count, uint64_t *checksum) {
    if (strncmp(line, "END,count=", 10) != 0) return false;
    const char *middle = strstr(line + 10, ",fnv1a64=");
    if (middle == NULL) return false;
    char count_text[32];
    size_t count_size = (size_t)(middle - (line + 10));
    if (count_size == 0 || count_size >= sizeof(count_text)) return false;
    memcpy(count_text, line + 10, count_size);
    count_text[count_size] = '\0';
    if (!decimal_u64(count_text, L1_TRACE_MAX_SAMPLES, count)) return false;
    const char *hex = middle + 9;
    if (strlen(hex) != 17 || hex[16] != '\n') return false;
    uint64_t value = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char c = (unsigned char)hex[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *checksum = value;
    return true;
}

bool l1_trace_read(const char *path, L1TraceRowFn callback, void *context,
                   L1TraceInfo *info, char *error, size_t error_size) {
    if (path == NULL || info == NULL) return false;
    memset(info, 0, sizeof(*info));
    struct stat metadata;
    if (stat(path, &metadata) != 0 || metadata.st_size <= 0 ||
        metadata.st_size > L1_TRACE_MAX_BYTES) {
        set_error(error, error_size, "trace size/path invalid");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "open failed: %s", strerror(errno));
        return false;
    }
    char line[L1_TRACE_MAX_LINE + 2];
    uint64_t hash = UINT64_C(1469598103934665603);
    static const char *fixed[] = {
        "HMI_L1_PSI_TRACE_V1\n", "schema=1\n",
        "units=relative_ms,basis_points,microseconds\n"
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (fgets(line, sizeof(line), file) == NULL ||
            !clean_line(line, strlen(line)) || strcmp(line, fixed[i]) != 0) {
            ok = false; break;
        }
        hash = l1_trace_hash_bytes(hash, line, strlen(line));
    }
    uint64_t cadence = 0, limit = 0;
    if (ok) {
        ok = fgets(line, sizeof(line), file) != NULL &&
            clean_line(line, strlen(line)) &&
            header_value(line, "cadence_ms=", L1_TRACE_MAX_CADENCE_MS, &cadence) &&
            cadence >= L1_TRACE_MIN_CADENCE_MS;
        if (ok) hash = l1_trace_hash_bytes(hash, line, strlen(line));
    }
    if (ok) {
        ok = fgets(line, sizeof(line), file) != NULL && clean_line(line, strlen(line));
        if (ok && strcmp(line, "limit=samples\n") == 0) info->sample_limited = true;
        else if (ok && strcmp(line, "limit=duration\n") == 0) info->sample_limited = false;
        else ok = false;
        if (ok) hash = l1_trace_hash_bytes(hash, line, strlen(line));
    }
    if (ok) {
        ok = fgets(line, sizeof(line), file) != NULL &&
            clean_line(line, strlen(line)) &&
            header_value(line, "limit_value=", 86400, &limit) && limit >= 2;
        if (ok) hash = l1_trace_hash_bytes(hash, line, strlen(line));
    }
    static const char columns[] =
        "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
        "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n";
    if (ok) {
        ok = fgets(line, sizeof(line), file) != NULL &&
            clean_line(line, strlen(line)) && strcmp(line, columns) == 0;
        if (ok) hash = l1_trace_hash_bytes(hash, line, strlen(line));
    }
    info->cadence_ms = (uint32_t)cadence;
    info->limit_value = (uint32_t)limit;
    uint64_t count = 0, previous_ms = 0, first_ms = 0;
    L1PressureCounters previous = {0};
    bool have_previous = false, have_first = false, footer_seen = false;
    while (ok && fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        if (!clean_line(line, length)) { ok = false; break; }
        if (strncmp(line, "END,", 4) == 0) {
            uint64_t footer_count = 0, footer_hash = 0;
            ok = parse_footer(line, &footer_count, &footer_hash) &&
                footer_count == count && footer_hash == hash;
            info->checksum = footer_hash;
            footer_seen = true;
            break;
        }
        L1TraceRow row;
        if (!parse_row(line, &row) || count >= L1_TRACE_MAX_SAMPLES ||
            (!have_first && row.relative_ms != 0) ||
            (have_first && row.relative_ms <= previous_ms)) {
            ok = false; break;
        }
        if (row.observation.valid && have_previous &&
            (row.counters.cpu_some_total_us < previous.cpu_some_total_us ||
             row.counters.memory_full_total_us < previous.memory_full_total_us ||
             row.counters.io_full_total_us < previous.io_full_total_us)) {
            ok = false; break;
        }
        if (row.observation.valid) {
            previous = row.counters;
            have_previous = true;
        } else {
            have_previous = false;
        }
        if (!have_first) { first_ms = row.relative_ms; have_first = true; }
        previous_ms = row.relative_ms;
        hash = l1_trace_hash_bytes(hash, line, length);
        count++;
        if (callback != NULL && !callback(&row, context)) { ok = false; break; }
    }
    if (ok && (!footer_seen || count < 2 || fgetc(file) != EOF)) ok = false;
    if (ok && info->sample_limited && count != limit) ok = false;
    if (ok && !info->sample_limited && previous_ms < limit * 1000U) ok = false;
    info->count = count;
    info->bytes = (uint64_t)metadata.st_size;
    info->horizon_eligible = have_first && previous_ms - first_ms >= 60000U;
    if (fclose(file) != 0) ok = false;
    if (!ok) set_error(error, error_size, "invalid or incomplete V1 trace");
    return ok;
}
