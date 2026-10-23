#ifndef HMI_L1_TRACE_H
#define HMI_L1_TRACE_H

#include "l1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    L1_TRACE_MAX_BYTES = 16777216,
    L1_TRACE_MAX_LINE = 512,
    L1_TRACE_MAX_SAMPLES = 86400,
    L1_TRACE_MIN_CADENCE_MS = 1000,
    L1_TRACE_MAX_CADENCE_MS = 60000
};

typedef struct {
    uint64_t relative_ms;
    L1Observation observation;
    L1PressureCounters counters;
    uint8_t flags;
} L1TraceRow;

typedef struct {
    uint32_t cadence_ms;
    bool sample_limited;
    uint32_t limit_value;
    uint64_t count;
    uint64_t checksum;
    uint64_t bytes;
    bool horizon_eligible;
} L1TraceInfo;

typedef bool (*L1TraceRowFn)(const L1TraceRow *row, void *context);

uint64_t l1_trace_hash_bytes(uint64_t hash, const void *data, size_t size);
bool l1_trace_read(const char *path, L1TraceRowFn callback, void *context,
                   L1TraceInfo *info, char *error, size_t error_size);

#endif
