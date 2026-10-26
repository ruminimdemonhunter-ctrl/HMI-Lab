#include "l1_perception.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void expect(L1Observation observation, L1PerceptionInputQuality quality,
                   L1PerceptionState state, L1PerceptionInputQuality out_quality,
                   uint8_t mask) {
    L1Observation unchanged = observation;
    L1PerceptionResult first = l1_perception_classify(observation, quality);
    CHECK(first.state == state);
    CHECK(first.quality == out_quality);
    CHECK(first.pressure_mask == mask);
    CHECK(memcmp(&observation, &unchanged, sizeof(observation)) == 0);
    for (size_t i = 0; i < 1000; ++i) {
        L1PerceptionResult repeated = l1_perception_classify(observation, quality);
        CHECK(repeated.state == first.state);
        CHECK(repeated.quality == first.quality);
        CHECK(repeated.pressure_mask == first.pressure_mask);
    }
}

static L1Observation observation(uint16_t cpu, uint16_t memory, uint16_t io,
                                 bool valid) {
    L1Observation value = {cpu, memory, io, valid};
    return value;
}

static void test_all_masks_and_boundaries(void) {
    static const struct {
        uint16_t cpu;
        uint16_t memory;
        uint16_t io;
        L1PerceptionState state;
        uint8_t mask;
    } cases[] = {
        {0, 0, 0, L1_PS_QUIET, 0x00},
        {3000, 0, 0, L1_PS_CPU_PRESSURE, 0x01},
        {0, 100, 0, L1_PS_MEMORY_PRESSURE, 0x02},
        {3000, 100, 0, L1_PS_MIXED_PRESSURE, 0x03},
        {0, 0, 500, L1_PS_IO_PRESSURE, 0x04},
        {3000, 0, 500, L1_PS_MIXED_PRESSURE, 0x05},
        {0, 100, 500, L1_PS_MIXED_PRESSURE, 0x06},
        {3000, 100, 500, L1_PS_MIXED_PRESSURE, 0x07}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        expect(observation(cases[i].cpu, cases[i].memory, cases[i].io, true),
               L1_PQ_OK, cases[i].state, L1_PQ_OK, cases[i].mask);
    }

    expect(observation(2999, 99, 499, true), L1_PQ_OK,
           L1_PS_QUIET, L1_PQ_OK, 0);
    expect(observation(2999, 0, 0, true), L1_PQ_OK,
           L1_PS_QUIET, L1_PQ_OK, 0);
    expect(observation(3001, 0, 0, true), L1_PQ_OK,
           L1_PS_CPU_PRESSURE, L1_PQ_OK, 1);
    expect(observation(0, 99, 0, true), L1_PQ_OK,
           L1_PS_QUIET, L1_PQ_OK, 0);
    expect(observation(0, 101, 0, true), L1_PQ_OK,
           L1_PS_MEMORY_PRESSURE, L1_PQ_OK, 2);
    expect(observation(0, 0, 499, true), L1_PQ_OK,
           L1_PS_QUIET, L1_PQ_OK, 0);
    expect(observation(0, 0, 501, true), L1_PQ_OK,
           L1_PS_IO_PRESSURE, L1_PQ_OK, 4);
    expect(observation(3000, 100, 500, true), L1_PQ_OK,
           L1_PS_MIXED_PRESSURE, L1_PQ_OK, 7);
    expect(observation(3001, 101, 501, true), L1_PQ_OK,
           L1_PS_MIXED_PRESSURE, L1_PQ_OK, 7);
    expect(observation(10000, 10000, 10000, true), L1_PQ_OK,
           L1_PS_MIXED_PRESSURE, L1_PQ_OK, 7);
    expect(observation(10000, 0, 0, true), L1_PQ_OK,
           L1_PS_CPU_PRESSURE, L1_PQ_OK, 1);
    expect(observation(0, 10000, 0, true), L1_PQ_OK,
           L1_PS_MEMORY_PRESSURE, L1_PQ_OK, 2);
    expect(observation(0, 0, 10000, true), L1_PQ_OK,
           L1_PS_IO_PRESSURE, L1_PQ_OK, 4);
    expect(observation(10001, 0, 0, true), L1_PQ_OK,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(observation(0, 10001, 0, true), L1_PQ_OK,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(observation(0, 0, 10001, true), L1_PQ_OK,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(observation(3000, 100, 500, false), L1_PQ_OK,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
}

static void test_quality_contract(void) {
    L1Observation fields_ignored = observation(65535, 65535, 65535, false);
    expect(fields_ignored, L1_PQ_ABSENT,
           L1_PS_UNKNOWN, L1_PQ_ABSENT, 0);
    expect(fields_ignored, L1_PQ_INVALID,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(fields_ignored, L1_PQ_STALE,
           L1_PS_UNRELIABLE, L1_PQ_STALE, 0);
    expect(fields_ignored, L1_PQ_REGRESSED,
           L1_PS_UNRELIABLE, L1_PQ_REGRESSED, 0);
    expect(fields_ignored, L1_PQ_RESET,
           L1_PS_UNRELIABLE, L1_PQ_RESET, 0);
    expect(observation(0, 0, 0, true), L1_PQ_OK,
           L1_PS_QUIET, L1_PQ_OK, 0);
    expect(fields_ignored, (L1PerceptionInputQuality)-1,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(fields_ignored, (L1PerceptionInputQuality)6,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    expect(fields_ignored, (L1PerceptionInputQuality)127,
           L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
}

static void test_core_isolation(void) {
    static const struct {
        uint64_t time_ms;
        L1Observation observation;
        L1PerceptionInputQuality quality;
    } sequence[] = {
        {0,    {0,    0,   0,   true},  L1_PQ_OK},
        {1000, {4000, 0,   0,   true},  L1_PQ_OK},
        {2000, {4000, 0,   0,   true},  L1_PQ_STALE},
        {3000, {0,    0,   900, true},  L1_PQ_OK},
        {4000, {0,    0,   900, true},  L1_PQ_RESET},
        {5000, {4000, 200, 900, true},  L1_PQ_OK},
        {6000, {4000, 200, 900, true},  L1_PQ_ABSENT},
        {7000, {4000, 200, 900, false}, L1_PQ_INVALID},
        {6500, {4000, 0,   0,   true},  L1_PQ_REGRESSED},
        {8000, {0,    0,   0,   true},  L1_PQ_OK}
    };
    L1Core perceived;
    L1Core reference;
    l1_init(&perceived);
    l1_init(&reference);

    CHECK(sizeof(L1Core) == 104);
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        L1Observation copy = sequence[i].observation;
        (void)l1_perception_classify(copy, sequence[i].quality);
        L1Action with_perception = l1_step_at(
            &perceived, sequence[i].observation, sequence[i].time_ms);
        L1Action without_perception = l1_step_at(
            &reference, sequence[i].observation, sequence[i].time_ms);
        CHECK(with_perception == without_perception);
        CHECK(memcmp(&perceived, &reference, sizeof(perceived)) == 0);
        CHECK(memcmp(&copy, &sequence[i].observation, sizeof(copy)) == 0);
    }
}

int main(void) {
    test_all_masks_and_boundaries();
    test_quality_contract();
    test_core_isolation();
    puts("PASS: R2.1 stateless perception masks, quality, boundaries, determinism, and core byte isolation");
    return EXIT_SUCCESS;
}
