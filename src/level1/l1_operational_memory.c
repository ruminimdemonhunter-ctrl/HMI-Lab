#include "l1_operational_memory.h"

#include <limits.h>
#include <string.h>

enum {
    L1_OM_CPU = 0,
    L1_OM_MEMORY = 1,
    L1_OM_IO = 2,
    L1_OM_VERSION = 1
};

static const uint8_t checkpoint_magic[8] = {
    'H', 'M', 'I', '_', 'L', '1', 'M', '1'
};

static uint32_t saturating_add_u32(uint32_t left, uint32_t right) {
    return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}

static void increment_u32(uint32_t *value) {
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static void increment_u16(uint16_t *value) {
    if (*value != UINT16_MAX) {
        ++*value;
    }
}

static uint32_t bin_sum(const uint16_t bins[L1_OM_BINS]) {
    uint32_t sum = 0;
    for (size_t index = 0; index < L1_OM_BINS; ++index) {
        sum += bins[index];
    }
    return sum;
}

static uint8_t quantize(uint16_t value, uint32_t threshold) {
    uint32_t quarter = (threshold + 3U) / 4U;
    uint32_t half = (threshold + 1U) / 2U;
    uint32_t wide_value = value;
    if (wide_value == 0U) {
        return 0;
    }
    if (wide_value < quarter) {
        return 1;
    }
    if (wide_value < half) {
        return 2;
    }
    if (wide_value < threshold) {
        return 3;
    }
    if (wide_value < threshold * 2U) {
        return 4;
    }
    if (wide_value < threshold * 4U) {
        return 5;
    }
    if (wide_value < threshold * 8U) {
        return 6;
    }
    return 7;
}

static uint32_t aggregate_decay(const L1OperationalMemory *memory) {
    uint32_t total = memory->imported_decay_count;
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        total = saturating_add_u32(total, memory->decay_count[channel]);
    }
    return total;
}

static void reset_with_status(L1OperationalMemory *memory,
                              L1OperationalMemoryStatus status) {
    memset(memory, 0, sizeof(*memory));
    memory->status = status;
}

void l1_operational_memory_init(L1OperationalMemory *memory) {
    if (memory != NULL) {
        reset_with_status(memory, L1_OM_WARMING_UP);
    }
}

static bool recognized_quality(L1PerceptionInputQuality quality) {
    switch (quality) {
        case L1_PQ_OK:
        case L1_PQ_ABSENT:
        case L1_PQ_INVALID:
        case L1_PQ_STALE:
        case L1_PQ_REGRESSED:
        case L1_PQ_RESET:
            return true;
        default:
            return false;
    }
}

static void update_status_after_accept(L1OperationalMemory *memory) {
    if (memory->restored) {
        if (memory->refresh_accepted >= L1_OM_REFRESH_SAMPLES &&
            memory->refresh_windows >= L1_OM_REFRESH_WINDOWS) {
            memory->restored = false;
        } else {
            memory->status = L1_OM_RESTORED_UNVERIFIED;
            return;
        }
    }
    memory->status =
        memory->accepted_lifetime >= L1_OM_MIN_SUPPORT_SAMPLES &&
        memory->windows_seen >= L1_OM_MIN_SUPPORT_WINDOWS
            ? L1_OM_SUPPORTED : L1_OM_WARMING_UP;
}

L1OperationalMemoryStatus l1_operational_memory_step(
    L1OperationalMemory *memory, L1Observation observation,
    L1PerceptionInputQuality quality, uint64_t monotonic_ms) {
    if (memory == NULL) {
        return L1_OM_INVALID;
    }
    if (quality == L1_PQ_REGRESSED || quality == L1_PQ_RESET) {
        reset_with_status(memory, L1_OM_RESET);
        return memory->status;
    }
    if (!recognized_quality(quality) || quality != L1_PQ_OK ||
        !observation.valid || observation.cpu_some_bp > 10000U ||
        observation.memory_full_bp > 10000U ||
        observation.io_full_bp > 10000U) {
        increment_u32(&memory->rejected_lifetime);
        memory->status = L1_OM_INVALID;
        return memory->status;
    }
    if (memory->monotonic_initialized) {
        if (monotonic_ms < memory->last_accepted_ms) {
            reset_with_status(memory, L1_OM_RESET);
            return memory->status;
        }
        if (monotonic_ms - memory->last_accepted_ms < L1_OM_SAMPLE_INTERVAL_MS) {
            increment_u32(&memory->rejected_lifetime);
            memory->status = L1_OM_INVALID;
            return memory->status;
        }
    }

    uint64_t bucket = monotonic_ms / L1_OM_WINDOW_MS;
    if (!memory->monotonic_initialized || bucket != memory->last_bucket) {
        increment_u16(&memory->windows_seen);
        memory->last_bucket = bucket;
    }
    memory->monotonic_initialized = true;
    memory->last_accepted_ms = monotonic_ms;

    if (memory->restored) {
        increment_u32(&memory->refresh_accepted);
        if (!memory->refresh_bucket_initialized ||
            bucket != memory->refresh_last_bucket) {
            increment_u16(&memory->refresh_windows);
            memory->refresh_last_bucket = bucket;
            memory->refresh_bucket_initialized = true;
        }
    }

    const uint16_t values[L1_OM_CHANNELS] = {
        observation.cpu_some_bp,
        observation.memory_full_bp,
        observation.io_full_bp
    };
    const uint32_t thresholds[L1_OM_CHANNELS] = {3000U, 100U, 500U};
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        if (bin_sum(memory->bins[channel]) >= L1_OM_DECAY_SUM) {
            for (size_t bin = 0; bin < L1_OM_BINS; ++bin) {
                memory->bins[channel][bin] /= 2U;
            }
            increment_u32(&memory->decay_count[channel]);
        }
        uint8_t bin = quantize(values[channel], thresholds[channel]);
        if (memory->bins[channel][bin] != UINT16_MAX) {
            ++memory->bins[channel][bin];
        }
    }
    increment_u32(&memory->accepted_lifetime);
    update_status_after_accept(memory);
    return memory->status;
}

L1OperationalMemorySummary l1_operational_memory_summary(
    const L1OperationalMemory *memory) {
    L1OperationalMemorySummary summary;
    memset(&summary, 0, sizeof(summary));
    if (memory == NULL) {
        summary.status = L1_OM_INVALID;
        return summary;
    }
    memcpy(summary.bins, memory->bins, sizeof(summary.bins));
    summary.accepted_lifetime = memory->accepted_lifetime;
    summary.rejected_lifetime = memory->rejected_lifetime;
    memcpy(summary.decay_count, memory->decay_count,
           sizeof(summary.decay_count));
    summary.aggregate_decay_count = aggregate_decay(memory);
    summary.refresh_accepted = memory->refresh_accepted;
    summary.windows_seen = memory->windows_seen;
    summary.refresh_windows = memory->refresh_windows;
    summary.status = memory->status;
    return summary;
}

static void put_u16(uint8_t *output, size_t offset, uint16_t value) {
    output[offset] = (uint8_t)(value & 0xffU);
    output[offset + 1U] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *output, size_t offset, uint32_t value) {
    output[offset] = (uint8_t)(value & 0xffU);
    output[offset + 1U] = (uint8_t)((value >> 8U) & 0xffU);
    output[offset + 2U] = (uint8_t)((value >> 16U) & 0xffU);
    output[offset + 3U] = (uint8_t)(value >> 24U);
}

static uint16_t get_u16(const uint8_t *input, size_t offset) {
    return (uint16_t)((uint16_t)input[offset] |
                      (uint16_t)((uint16_t)input[offset + 1U] << 8U));
}

static uint32_t get_u32(const uint8_t *input, size_t offset) {
    return (uint32_t)input[offset] |
           ((uint32_t)input[offset + 1U] << 8U) |
           ((uint32_t)input[offset + 2U] << 16U) |
           ((uint32_t)input[offset + 3U] << 24U);
}

static uint32_t crc32(const uint8_t *bytes, size_t length) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static bool state_valid_for_checkpoint(const L1OperationalMemory *memory) {
    bool any = false;
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        uint32_t sum = bin_sum(memory->bins[channel]);
        if (sum > L1_OM_DECAY_SUM || sum > memory->accepted_lifetime) {
            return false;
        }
        if (memory->accepted_lifetime != 0U && sum == 0U) {
            return false;
        }
        any = any || sum != 0U;
    }
    if (memory->windows_seen > memory->accepted_lifetime) {
        return false;
    }
    if (memory->accepted_lifetime != 0U &&
        (!any || memory->windows_seen == 0U)) {
        return false;
    }
    return true;
}

bool l1_operational_memory_encode(const L1OperationalMemory *memory,
                                  uint8_t *output, size_t output_size) {
    if (output != NULL && output_size >= L1_OM_CHECKPOINT_BYTES) {
        memset(output, 0, L1_OM_CHECKPOINT_BYTES);
    }
    if (memory == NULL || output == NULL ||
        output_size != L1_OM_CHECKPOINT_BYTES ||
        !state_valid_for_checkpoint(memory)) {
        return false;
    }
    memcpy(output, checkpoint_magic, sizeof(checkpoint_magic));
    put_u16(output, 8, L1_OM_VERSION);
    put_u16(output, 10, L1_OM_CHECKPOINT_BYTES);
    size_t offset = 12;
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        for (size_t bin = 0; bin < L1_OM_BINS; ++bin) {
            put_u16(output, offset, memory->bins[channel][bin]);
            offset += 2U;
        }
    }
    put_u32(output, 60, memory->accepted_lifetime);
    put_u16(output, 64, memory->windows_seen);
    put_u16(output, 66, 0);
    put_u32(output, 68, memory->rejected_lifetime);
    put_u32(output, 72, aggregate_decay(memory));
    put_u32(output, 76, crc32(output, 76));
    return true;
}

bool l1_operational_memory_decode(L1OperationalMemory *memory,
                                  const uint8_t *input, size_t input_size) {
    if (memory == NULL) {
        return false;
    }
    l1_operational_memory_init(memory);
    if (input == NULL || input_size != L1_OM_CHECKPOINT_BYTES ||
        memcmp(input, checkpoint_magic, sizeof(checkpoint_magic)) != 0 ||
        get_u16(input, 8) != L1_OM_VERSION ||
        get_u16(input, 10) != L1_OM_CHECKPOINT_BYTES ||
        get_u16(input, 66) != 0U ||
        get_u32(input, 76) != crc32(input, 76)) {
        return false;
    }

    size_t offset = 12;
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        for (size_t bin = 0; bin < L1_OM_BINS; ++bin) {
            memory->bins[channel][bin] = get_u16(input, offset);
            offset += 2U;
        }
    }
    memory->accepted_lifetime = get_u32(input, 60);
    memory->windows_seen = get_u16(input, 64);
    memory->rejected_lifetime = get_u32(input, 68);
    memory->imported_decay_count = get_u32(input, 72);
    if (!state_valid_for_checkpoint(memory)) {
        l1_operational_memory_init(memory);
        return false;
    }
    memory->restored = true;
    memory->status = L1_OM_RESTORED_UNVERIFIED;
    return true;
}
