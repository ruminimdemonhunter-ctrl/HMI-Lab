#ifndef HMI_L1_OPERATIONAL_MEMORY_H
#define HMI_L1_OPERATIONAL_MEMORY_H

#include "l1_perception.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    L1_OM_CHANNELS = 3,
    L1_OM_BINS = 8,
    L1_OM_CHECKPOINT_BYTES = 80,
    L1_OM_MIN_SUPPORT_SAMPLES = 256,
    L1_OM_MIN_SUPPORT_WINDOWS = 4,
    L1_OM_REFRESH_SAMPLES = 128,
    L1_OM_REFRESH_WINDOWS = 2,
    L1_OM_SAMPLE_INTERVAL_MS = 1000,
    L1_OM_WINDOW_MS = 300000,
    L1_OM_DECAY_SUM = 1024
};

typedef enum {
    L1_OM_WARMING_UP = 0,
    L1_OM_SUPPORTED = 1,
    L1_OM_INVALID = 2,
    L1_OM_RESET = 3,
    L1_OM_RESTORED_UNVERIFIED = 4
} L1OperationalMemoryStatus;

typedef struct {
    uint16_t bins[L1_OM_CHANNELS][L1_OM_BINS];
    uint32_t accepted_lifetime;
    uint32_t rejected_lifetime;
    uint32_t decay_count[L1_OM_CHANNELS];
    uint32_t imported_decay_count;
    uint64_t last_accepted_ms;
    uint64_t last_bucket;
    uint64_t refresh_last_bucket;
    uint32_t refresh_accepted;
    uint16_t windows_seen;
    uint16_t refresh_windows;
    L1OperationalMemoryStatus status;
    bool monotonic_initialized;
    bool refresh_bucket_initialized;
    bool restored;
} L1OperationalMemory;

typedef struct {
    uint16_t bins[L1_OM_CHANNELS][L1_OM_BINS];
    uint32_t accepted_lifetime;
    uint32_t rejected_lifetime;
    uint32_t decay_count[L1_OM_CHANNELS];
    uint32_t aggregate_decay_count;
    uint32_t refresh_accepted;
    uint16_t windows_seen;
    uint16_t refresh_windows;
    L1OperationalMemoryStatus status;
} L1OperationalMemorySummary;

void l1_operational_memory_init(L1OperationalMemory *memory);
L1OperationalMemoryStatus l1_operational_memory_step(
    L1OperationalMemory *memory, L1Observation observation,
    L1PerceptionInputQuality quality, uint64_t monotonic_ms);
L1OperationalMemorySummary l1_operational_memory_summary(
    const L1OperationalMemory *memory);
bool l1_operational_memory_encode(const L1OperationalMemory *memory,
                                  uint8_t *output, size_t output_size);
bool l1_operational_memory_decode(L1OperationalMemory *memory,
                                  const uint8_t *input, size_t input_size);

#endif
