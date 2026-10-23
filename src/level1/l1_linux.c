#define _POSIX_C_SOURCE 200809L
#include "l1_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool reader_open_paths(L1LinuxPsiReader *reader, const char *cpu_path,
                              const char *memory_path, const char *io_path) {
    if (reader == NULL || cpu_path == NULL || memory_path == NULL ||
        io_path == NULL) return false;
    *reader = (L1LinuxPsiReader){-1, -1, -1};
    reader->cpu_fd = open(cpu_path, O_RDONLY | O_CLOEXEC);
    reader->memory_fd = open(memory_path, O_RDONLY | O_CLOEXEC);
    reader->io_fd = open(io_path, O_RDONLY | O_CLOEXEC);
    if (reader->cpu_fd < 0 || reader->memory_fd < 0 || reader->io_fd < 0) {
        l1_linux_psi_reader_close(reader);
        return false;
    }
    return true;
}

bool l1_linux_psi_reader_open(L1LinuxPsiReader *reader) {
    return reader_open_paths(reader, "/proc/pressure/cpu",
                             "/proc/pressure/memory", "/proc/pressure/io");
}

void l1_linux_psi_reader_close(L1LinuxPsiReader *reader) {
    if (reader == NULL) return;
    if (reader->cpu_fd >= 0) (void)close(reader->cpu_fd);
    if (reader->memory_fd >= 0) (void)close(reader->memory_fd);
    if (reader->io_fd >= 0) (void)close(reader->io_fd);
    *reader = (L1LinuxPsiReader){-1, -1, -1};
}

static bool read_psi_fd(int descriptor, const char *category, uint16_t *bp,
                        uint64_t *total_us) {
    char buffer[1024];
    ssize_t count = pread(descriptor, buffer, sizeof(buffer) - 1U, 0);
    if (count <= 0 || (size_t)count >= sizeof(buffer) - 1U) return false;
    buffer[count] = '\0';
    char *line = buffer;
    while (*line != '\0') {
        char *newline = strchr(line, '\n');
        if (newline == NULL) return false;
        char *next = newline + 1;
        *newline = '\0';
        if (l1_parse_psi_sample(line, category, bp, total_us)) return true;
        line = next;
    }
    return false;
}

bool l1_linux_psi_reader_read(L1LinuxPsiReader *reader, L1Observation *out,
                              L1PressureCounters *counters) {
    if (reader == NULL || out == NULL || counters == NULL) return false;
    *out = (L1Observation){0};
    *counters = (L1PressureCounters){0};
    out->valid = read_psi_fd(reader->cpu_fd, "some", &out->cpu_some_bp,
                             &counters->cpu_some_total_us) &&
                 read_psi_fd(reader->memory_fd, "full", &out->memory_full_bp,
                             &counters->memory_full_total_us) &&
                 read_psi_fd(reader->io_fd, "full", &out->io_full_bp,
                             &counters->io_full_total_us);
    counters->valid = out->valid;
    return out->valid;
}

#ifdef L1_TESTING
bool l1_linux_psi_reader_open_paths(L1LinuxPsiReader *reader,
                                    const char *cpu_path,
                                    const char *memory_path,
                                    const char *io_path) {
    return reader_open_paths(reader, cpu_path, memory_path, io_path);
}

bool l1_read_linux_sample_paths(const char *cpu_path, const char *memory_path,
                                const char *io_path, L1Observation *out,
                                L1PressureCounters *counters) {
    if (out == NULL || counters == NULL) return false;
    *out = (L1Observation){0};
    *counters = (L1PressureCounters){0};
    out->valid = read_psi(cpu_path, "some", &out->cpu_some_bp,
                          &counters->cpu_some_total_us) &&
                 read_psi(memory_path, "full", &out->memory_full_bp,
                          &counters->memory_full_total_us) &&
                 read_psi(io_path, "full", &out->io_full_bp,
                          &counters->io_full_total_us);
    counters->valid = out->valid;
    return out->valid;
}
#endif

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
