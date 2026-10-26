#include "l1_operational_memory.h"
#include "l1_shadow.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { CPU = 0, MEMORY = 1, IO = 2 };

static L1Observation observed(uint16_t cpu, uint16_t memory, uint16_t io,
                              bool valid) {
    L1Observation value = {cpu, memory, io, valid};
    return value;
}

static uint32_t sum_bins(const uint16_t bins[L1_OM_BINS]) {
    uint32_t sum = 0;
    for (size_t i = 0; i < L1_OM_BINS; ++i) {
        sum += bins[i];
    }
    return sum;
}

static void expect_bin(size_t channel, uint16_t value, uint8_t expected_bin) {
    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    uint16_t values[3] = {0, 0, 0};
    values[channel] = value;
    CHECK(l1_operational_memory_step(
        &memory, observed(values[0], values[1], values[2], true),
        L1_PQ_OK, 0) == L1_OM_WARMING_UP);
    L1OperationalMemorySummary summary = l1_operational_memory_summary(&memory);
    CHECK(summary.bins[channel][expected_bin] == 1);
    CHECK(sum_bins(summary.bins[channel]) == 1);
}

static void test_quantization(void) {
    static const struct { uint16_t value; uint8_t bin; } cpu[] = {
        {0, 0}, {1, 1}, {749, 1}, {750, 2}, {1499, 2}, {1500, 3},
        {2999, 3}, {3000, 4}, {5999, 4}, {6000, 5}, {10000, 5}
    };
    static const struct { uint16_t value; uint8_t bin; } memory[] = {
        {0, 0}, {1, 1}, {24, 1}, {25, 2}, {49, 2}, {50, 3},
        {99, 3}, {100, 4}, {199, 4}, {200, 5}, {399, 5},
        {400, 6}, {799, 6}, {800, 7}, {10000, 7}
    };
    static const struct { uint16_t value; uint8_t bin; } io[] = {
        {0, 0}, {1, 1}, {124, 1}, {125, 2}, {249, 2}, {250, 3},
        {499, 3}, {500, 4}, {999, 4}, {1000, 5}, {1999, 5},
        {2000, 6}, {3999, 6}, {4000, 7}, {10000, 7}
    };
    for (size_t i = 0; i < sizeof(cpu) / sizeof(cpu[0]); ++i) {
        expect_bin(CPU, cpu[i].value, cpu[i].bin);
    }
    for (size_t i = 0; i < sizeof(memory) / sizeof(memory[0]); ++i) {
        expect_bin(MEMORY, memory[i].value, memory[i].bin);
    }
    for (size_t i = 0; i < sizeof(io) / sizeof(io[0]); ++i) {
        expect_bin(IO, io[i].value, io[i].bin);
    }

    L1OperationalMemory memory_state;
    l1_operational_memory_init(&memory_state);
    CHECK(l1_operational_memory_step(&memory_state,
        observed(10001, 0, 0, true), L1_PQ_OK, 0) == L1_OM_INVALID);
    CHECK(l1_operational_memory_summary(&memory_state).accepted_lifetime == 0);
    l1_operational_memory_init(&memory_state);
    CHECK(l1_operational_memory_step(&memory_state,
        observed(0, 10001, 0, true), L1_PQ_OK, 0) == L1_OM_INVALID);
    l1_operational_memory_init(&memory_state);
    CHECK(l1_operational_memory_step(&memory_state,
        observed(0, 0, 10001, true), L1_PQ_OK, 0) == L1_OM_INVALID);
    L1OperationalMemorySummary summary = l1_operational_memory_summary(&memory_state);
    CHECK(summary.bins[CPU][6] == 0 && summary.bins[CPU][7] == 0);
}

static void test_rejection_and_time(void) {
    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    L1Observation valid = observed(3000, 100, 500, true);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 1000) ==
          L1_OM_WARMING_UP);
    L1OperationalMemory before = memory;
    const L1PerceptionInputQuality rejected[] = {
        L1_PQ_ABSENT, L1_PQ_INVALID, L1_PQ_STALE,
        (L1PerceptionInputQuality)-1, (L1PerceptionInputQuality)6
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        uint32_t prior_rejected = memory.rejected_lifetime;
        CHECK(l1_operational_memory_step(&memory, valid, rejected[i],
                                         2000 + i * 1000) == L1_OM_INVALID);
        CHECK(memory.accepted_lifetime == before.accepted_lifetime);
        CHECK(memcmp(memory.bins, before.bins, sizeof(memory.bins)) == 0);
        CHECK(memory.rejected_lifetime == prior_rejected + 1U);
    }
    uint32_t prior_rejected = memory.rejected_lifetime;
    CHECK(l1_operational_memory_step(&memory, observed(0, 0, 0, false),
                                     L1_PQ_OK, 8000) == L1_OM_INVALID);
    CHECK(memory.rejected_lifetime == prior_rejected + 1U);

    l1_operational_memory_init(&memory);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 1000) ==
          L1_OM_WARMING_UP);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 1000) ==
          L1_OM_INVALID);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 1999) ==
          L1_OM_INVALID);
    CHECK(memory.accepted_lifetime == 1 && memory.rejected_lifetime == 2);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 2000) ==
          L1_OM_WARMING_UP);
    CHECK(memory.accepted_lifetime == 2);

    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 1999) ==
          L1_OM_RESET);
    CHECK(memory.accepted_lifetime == 0 && memory.windows_seen == 0);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 5000) ==
          L1_OM_WARMING_UP);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_REGRESSED, 6000) ==
          L1_OM_RESET);
    CHECK(memory.accepted_lifetime == 0);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_OK, 7000) ==
          L1_OM_WARMING_UP);
    CHECK(l1_operational_memory_step(&memory, valid, L1_PQ_RESET, 8000) ==
          L1_OM_RESET);
}

static uint64_t spaced_time(size_t index, size_t samples_per_bucket) {
    size_t bucket = index / samples_per_bucket;
    size_t within = index % samples_per_bucket;
    return (uint64_t)bucket * L1_OM_WINDOW_MS +
           (uint64_t)within * L1_OM_SAMPLE_INTERVAL_MS;
}

static void test_support_windows_and_gaps(void) {
    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    for (size_t i = 0; i < 255; ++i) {
        CHECK(l1_operational_memory_step(&memory, observed(1000, 20, 200, true),
                                         L1_PQ_OK, spaced_time(i, 64)) ==
              L1_OM_WARMING_UP);
    }
    CHECK(memory.accepted_lifetime == 255 && memory.windows_seen == 4);
    CHECK(l1_operational_memory_step(&memory, observed(1000, 20, 200, true),
                                     L1_PQ_OK, spaced_time(255, 64)) ==
          L1_OM_SUPPORTED);

    l1_operational_memory_init(&memory);
    for (size_t i = 0; i < 256; ++i) {
        CHECK(l1_operational_memory_step(&memory, observed(1000, 20, 200, true),
                                         L1_PQ_OK, spaced_time(i, 100)) ==
              L1_OM_WARMING_UP);
    }
    CHECK(memory.accepted_lifetime == 256 && memory.windows_seen == 3);
    CHECK(l1_operational_memory_step(&memory, observed(1000, 20, 200, true),
                                     L1_PQ_OK, 3U * L1_OM_WINDOW_MS) ==
          L1_OM_SUPPORTED);
    CHECK(memory.windows_seen == 4);

    uint16_t windows = memory.windows_seen;
    CHECK(l1_operational_memory_step(&memory, observed(1000, 20, 200, true),
                                     L1_PQ_OK, UINT64_C(100) * L1_OM_WINDOW_MS) ==
          L1_OM_SUPPORTED);
    CHECK(memory.windows_seen == (uint16_t)(windows + 1U));
}

static void test_decay_saturation_and_regime_change(void) {
    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    for (size_t i = 0; i < 1025; ++i) {
        (void)l1_operational_memory_step(
            &memory, observed(0, 0, 0, true), L1_PQ_OK,
            (uint64_t)i * L1_OM_SAMPLE_INTERVAL_MS);
    }
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        CHECK(memory.bins[channel][0] == 513);
        CHECK(sum_bins(memory.bins[channel]) == 513);
        CHECK(memory.decay_count[channel] == 1);
    }
    for (size_t i = 1025; i < 1537; ++i) {
        (void)l1_operational_memory_step(
            &memory, observed(0, 0, 0, true), L1_PQ_OK,
            (uint64_t)i * L1_OM_SAMPLE_INTERVAL_MS);
    }
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        CHECK(memory.bins[channel][0] == 513);
        CHECK(memory.decay_count[channel] == 2);
    }

    for (size_t i = 1537; i < 1800; ++i) {
        (void)l1_operational_memory_step(
            &memory, observed(7000, 900, 5000, true), L1_PQ_OK,
            (uint64_t)i * L1_OM_SAMPLE_INTERVAL_MS);
    }
    CHECK(memory.bins[CPU][5] > 0);
    CHECK(memory.bins[MEMORY][7] > 0);
    CHECK(memory.bins[IO][7] > 0);

    memory.accepted_lifetime = UINT32_MAX;
    memory.rejected_lifetime = UINT32_MAX;
    memory.windows_seen = UINT16_MAX;
    memory.decay_count[0] = UINT32_MAX;
    memset(memory.bins[0], 0, sizeof(memory.bins[0]));
    memory.bins[0][0] = 1024;
    uint64_t next_time = memory.last_accepted_ms + L1_OM_WINDOW_MS;
    (void)l1_operational_memory_step(
        &memory, observed(0, 0, 0, true), L1_PQ_OK, next_time);
    CHECK(memory.accepted_lifetime == UINT32_MAX);
    CHECK(memory.windows_seen == UINT16_MAX);
    CHECK(memory.decay_count[0] == UINT32_MAX);
    (void)l1_operational_memory_step(
        &memory, observed(0, 0, 0, false), L1_PQ_OK,
        next_time + L1_OM_SAMPLE_INTERVAL_MS);
    CHECK(memory.rejected_lifetime == UINT32_MAX);
}

static uint32_t test_crc32(const uint8_t *bytes, size_t length) {
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

static void put_u16(uint8_t *bytes, size_t offset, uint16_t value) {
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1U] = (uint8_t)(value >> 8U);
    bytes[offset + 2U] = (uint8_t)(value >> 16U);
    bytes[offset + 3U] = (uint8_t)(value >> 24U);
}

static void repair_crc(uint8_t bytes[L1_OM_CHECKPOINT_BYTES]) {
    put_u32(bytes, 76, test_crc32(bytes, 76));
}

static void expect_decode_failure(const uint8_t *bytes, size_t length) {
    L1OperationalMemory memory;
    memset(&memory, 0xa5, sizeof(memory));
    CHECK(!l1_operational_memory_decode(&memory, bytes, length));
    L1OperationalMemory empty;
    l1_operational_memory_init(&empty);
    CHECK(memcmp(&memory, &empty, sizeof(memory)) == 0);
}

static void test_checkpoint_and_restore(void) {
    L1OperationalMemory source;
    l1_operational_memory_init(&source);
    for (size_t i = 0; i < 300; ++i) {
        (void)l1_operational_memory_step(
            &source, observed(3500, 150, 700, true), L1_PQ_OK,
            spaced_time(i, 64));
    }
    uint8_t encoded[L1_OM_CHECKPOINT_BYTES];
    CHECK(l1_operational_memory_encode(&source, encoded, sizeof(encoded)));
    CHECK(memcmp(encoded, "HMI_L1M1", 8) == 0);
    CHECK(encoded[8] == 1 && encoded[9] == 0);
    CHECK(encoded[10] == 80 && encoded[11] == 0);

    L1OperationalMemory restored;
    CHECK(l1_operational_memory_decode(&restored, encoded, sizeof(encoded)));
    CHECK(restored.status == L1_OM_RESTORED_UNVERIFIED);
    CHECK(!restored.monotonic_initialized && restored.restored);
    uint8_t reencoded[L1_OM_CHECKPOINT_BYTES];
    CHECK(l1_operational_memory_encode(&restored, reencoded,
                                       sizeof(reencoded)));
    CHECK(memcmp(encoded, reencoded, sizeof(encoded)) == 0);

    for (size_t i = 0; i < 127; ++i) {
        uint64_t time = spaced_time(i, 64);
        CHECK(l1_operational_memory_step(
            &restored, observed(3500, 150, 700, true), L1_PQ_OK, time) ==
              L1_OM_RESTORED_UNVERIFIED);
    }
    CHECK(restored.refresh_accepted == 127 && restored.refresh_windows == 2);
    CHECK(l1_operational_memory_step(
        &restored, observed(3500, 150, 700, true), L1_PQ_OK,
        spaced_time(127, 64)) == L1_OM_SUPPORTED);

    uint8_t corrupt[L1_OM_CHECKPOINT_BYTES + 1U];
    memcpy(corrupt, encoded, sizeof(encoded));
    corrupt[0] ^= 1U;
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    corrupt[8] = 2;
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    corrupt[10] = 79;
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    corrupt[66] = 1;
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    put_u16(corrupt, 12, 1025);
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    put_u32(corrupt, 60, 0);
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    memcpy(corrupt, encoded, sizeof(encoded));
    put_u16(corrupt, 64, UINT16_MAX);
    repair_crc(corrupt);
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES);
    expect_decode_failure(encoded, L1_OM_CHECKPOINT_BYTES - 1U);
    memcpy(corrupt, encoded, sizeof(encoded));
    corrupt[L1_OM_CHECKPOINT_BYTES] = 0;
    expect_decode_failure(corrupt, L1_OM_CHECKPOINT_BYTES + 1U);

    uint8_t wrong_size[L1_OM_CHECKPOINT_BYTES + 1U];
    memset(wrong_size, 0xa5, sizeof(wrong_size));
    CHECK(!l1_operational_memory_encode(&source, wrong_size,
                                        sizeof(wrong_size)));
    CHECK(!l1_operational_memory_encode(NULL, encoded, sizeof(encoded)));
}

static void test_determinism_and_distinct_histories(void) {
    L1OperationalMemory first, second, different;
    l1_operational_memory_init(&first);
    l1_operational_memory_init(&second);
    l1_operational_memory_init(&different);
    for (size_t i = 0; i < 600; ++i) {
        uint64_t time = spaced_time(i, 64);
        L1Observation history = observed(
            (uint16_t)(500 + i % 100), (uint16_t)(20 + i % 10),
            (uint16_t)(100 + i % 50), true);
        (void)l1_operational_memory_step(&first, history, L1_PQ_OK, time);
        (void)l1_operational_memory_step(&second, history, L1_PQ_OK, time);
        (void)l1_operational_memory_step(
            &different, observed(7000, 900, 5000, true), L1_PQ_OK, time);
        CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    }
    uint8_t a[L1_OM_CHECKPOINT_BYTES], b[L1_OM_CHECKPOINT_BYTES];
    CHECK(l1_operational_memory_encode(&first, a, sizeof(a)));
    CHECK(l1_operational_memory_encode(&second, b, sizeof(b)));
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    CHECK(memcmp(first.bins, different.bins, sizeof(first.bins)) != 0);
}

static void test_core_shadow_and_classifier_isolation(void) {
    static const struct {
        uint64_t time_ms;
        L1Observation observation;
        L1PerceptionInputQuality quality;
    } rows[] = {
        {0,    {0,    0,   0,   true},  L1_PQ_OK},
        {1000, {5000, 0,   0,   true},  L1_PQ_OK},
        {2000, {5000, 0,   0,   true},  L1_PQ_STALE},
        {3000, {0,    0,   900, true},  L1_PQ_OK},
        {4000, {0,    0,   900, true},  L1_PQ_RESET},
        {5000, {5000, 200, 900, true},  L1_PQ_OK},
        {6000, {5000, 200, 900, true},  L1_PQ_ABSENT},
        {7000, {5000, 200, 900, false}, L1_PQ_INVALID},
        {6500, {5000, 0,   0,   true},  L1_PQ_REGRESSED},
        {8000, {0,    0,   0,   true},  L1_PQ_OK}
    };
    L1Core enabled_core, reference_core;
    L1Shadow enabled_shadow, reference_shadow;
    L1OperationalMemory memory;
    l1_init(&enabled_core);
    l1_init(&reference_core);
    l1_shadow_init(&enabled_shadow, l1_default_forecast_thresholds());
    l1_shadow_init(&reference_shadow, l1_default_forecast_thresholds());
    l1_operational_memory_init(&memory);
    CHECK(sizeof(L1Core) == 104);
    CHECK(sizeof(L1Shadow) == 208);
    CHECK(sizeof(L1OperationalMemory) <= 512);

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        L1PerceptionResult before = l1_perception_classify(
            rows[i].observation, rows[i].quality);
        (void)l1_operational_memory_step(
            &memory, rows[i].observation, rows[i].quality, rows[i].time_ms);
        L1PerceptionResult after = l1_perception_classify(
            rows[i].observation, rows[i].quality);
        CHECK(before.state == after.state && before.quality == after.quality &&
              before.pressure_mask == after.pressure_mask);

        L1Action enabled_action = l1_step_at(
            &enabled_core, rows[i].observation, rows[i].time_ms);
        L1Action reference_action = l1_step_at(
            &reference_core, rows[i].observation, rows[i].time_ms);
        CHECK(enabled_action == reference_action);
        CHECK(memcmp(&enabled_core, &reference_core,
                     sizeof(enabled_core)) == 0);

        L1PressureCounters counters = {
            (uint64_t)i * 1000U, (uint64_t)i * 100U,
            (uint64_t)i * 500U, rows[i].observation.valid
        };
        (void)l1_shadow_step_at(&enabled_shadow, rows[i].observation,
                                counters, rows[i].time_ms);
        (void)l1_shadow_step_at(&reference_shadow, rows[i].observation,
                                counters, rows[i].time_ms);
        CHECK(memcmp(&enabled_shadow, &reference_shadow,
                     sizeof(enabled_shadow)) == 0);
    }
}

int main(void) {
    test_quantization();
    test_rejection_and_time();
    test_support_windows_and_gaps();
    test_decay_saturation_and_regime_change();
    test_checkpoint_and_restore();
    test_determinism_and_distinct_histories();
    test_core_shadow_and_classifier_isolation();
    puts("PASS: R2.2 quantization, bounded memory, support, decay, checkpoint, recovery, determinism, and protected-lane isolation");
    return EXIT_SUCCESS;
}
