#include "l1_novelty.h"
#include "l1_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static L1OperationalMemorySummary reference(void) {
    L1OperationalMemorySummary summary;
    memset(&summary, 0, sizeof(summary));
    summary.status = L1_OM_SUPPORTED;
    summary.accepted_lifetime = 256;
    summary.windows_seen = 4;
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        for (size_t bin = 0; bin < L1_OM_BINS; ++bin) {
            summary.bins[channel][bin] = (uint16_t)(bin + 1U);
        }
    }
    return summary;
}

static void test_integer_equation_and_threshold(void) {
    L1OperationalMemorySummary summary = reference();
    L1Observation observation = {0, 25, 4000, true};
    L1NoveltyResult result = l1_novelty_score(
        &summary, observation, L1_PQ_OK, 900);
    CHECK(result.bin[0] == 0 && result.bin[1] == 2 && result.bin[2] == 7);
    CHECK(result.channel_score[0] == 977);
    CHECK(result.channel_score[1] == 930);
    CHECK(result.channel_score[2] == 814);
    CHECK(result.novelty_score == 977);
    CHECK(result.disposition == L1_NOVELTY_SUSPECTED);
    result = l1_novelty_score(&summary, observation, L1_PQ_OK, 1001);
    CHECK(result.disposition == L1_NOVELTY_EXPECTED);
    result = l1_novelty_score(&summary, observation, L1_PQ_OK, 1024);
    CHECK(result.disposition == L1_NOVELTY_EXPECTED);
}

static void test_boundaries_and_abstention(void) {
    const uint16_t values[][3] = {
        {0,0,0}, {1,1,1}, {750,25,125}, {1500,50,250},
        {3000,100,500}, {6000,200,1000}, {10000,400,2000},
        {10000,800,4000}
    };
    const uint8_t expected[][3] = {
        {0,0,0}, {1,1,1}, {2,2,2}, {3,3,3},
        {4,4,4}, {5,5,5}, {5,6,6}, {5,7,7}
    };
    L1OperationalMemorySummary summary = reference();
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        L1Observation observation = {values[i][0], values[i][1],
                                     values[i][2], true};
        L1NoveltyResult result = l1_novelty_score(
            &summary, observation, L1_PQ_OK, 1024);
        CHECK(result.disposition == L1_NOVELTY_EXPECTED);
        CHECK(memcmp(result.bin, expected[i], sizeof(result.bin)) == 0);
    }
    const L1PerceptionInputQuality bad[] = {
        L1_PQ_ABSENT, L1_PQ_INVALID, L1_PQ_STALE,
        L1_PQ_REGRESSED, L1_PQ_RESET, (L1PerceptionInputQuality)-1
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                               bad[i], 512).disposition ==
              L1_NOVELTY_UNKNOWN);
    }
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,false},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    CHECK(l1_novelty_score(&summary, (L1Observation){10001,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    CHECK(l1_novelty_score(&summary, (L1Observation){0,10001,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,10001,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 1025).disposition == L1_NOVELTY_UNKNOWN);
    CHECK(l1_novelty_score(NULL, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
}

static void test_corrupt_or_unsupported_reference(void) {
    L1OperationalMemorySummary summary = reference();
    summary.status = L1_OM_WARMING_UP;
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    summary = reference();
    memset(summary.bins[1], 0, sizeof(summary.bins[1]));
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    summary = reference();
    summary.bins[2][0] = 1025;
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
    summary = reference();
    summary.accepted_lifetime = 10;
    CHECK(l1_novelty_score(&summary, (L1Observation){0,0,0,true},
                           L1_PQ_OK, 512).disposition == L1_NOVELTY_UNKNOWN);
}

static void test_byte_isolation(void) {
    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    for (size_t i = 0; i < 300; ++i) {
        (void)l1_operational_memory_step(
            &memory, (L1Observation){1200, 20, 100, true}, L1_PQ_OK,
            (uint64_t)(i / 75U) * L1_OM_WINDOW_MS +
            (uint64_t)(i % 75U) * L1_OM_SAMPLE_INTERVAL_MS);
    }
    L1OperationalMemorySummary summary = l1_operational_memory_summary(&memory);
    L1OperationalMemory memory_before = memory;
    L1OperationalMemorySummary summary_before = summary;
    L1Core enabled, reference_core;
    L1Shadow enabled_shadow, reference_shadow;
    l1_init(&enabled);
    l1_init(&reference_core);
    l1_shadow_init(&enabled_shadow, l1_default_forecast_thresholds());
    l1_shadow_init(&reference_shadow, l1_default_forecast_thresholds());
    for (size_t i = 0; i < 20; ++i) {
        L1Observation observation = {
            (uint16_t)(i * 503U % 10001U),
            (uint16_t)(i * 47U % 10001U),
            (uint16_t)(i * 211U % 10001U), true
        };
        L1PerceptionResult perception_before =
            l1_perception_classify(observation, L1_PQ_OK);
        (void)l1_novelty_score(&summary, observation, L1_PQ_OK, 768);
        L1PerceptionResult perception_after =
            l1_perception_classify(observation, L1_PQ_OK);
        CHECK(perception_before.state == perception_after.state);
        CHECK(perception_before.quality == perception_after.quality);
        CHECK(perception_before.pressure_mask == perception_after.pressure_mask);
        CHECK(l1_step_at(&enabled, observation, i * 1000U) ==
              l1_step_at(&reference_core, observation, i * 1000U));
        CHECK(memcmp(&enabled, &reference_core, sizeof(enabled)) == 0);
        L1PressureCounters counters = {i * 100U, i * 10U, i * 20U, true};
        (void)l1_shadow_step_at(&enabled_shadow, observation, counters,
                                i * 1000U);
        (void)l1_shadow_step_at(&reference_shadow, observation, counters,
                                i * 1000U);
        CHECK(memcmp(&enabled_shadow, &reference_shadow,
                     sizeof(enabled_shadow)) == 0);
    }
    CHECK(memcmp(&memory, &memory_before, sizeof(memory)) == 0);
    CHECK(memcmp(&summary, &summary_before, sizeof(summary)) == 0);
    CHECK(sizeof(L1Core) == 104 && sizeof(L1Shadow) == 208);
    CHECK(sizeof(L1OperationalMemory) == 112);
}

int main(void) {
    test_integer_equation_and_threshold();
    test_boundaries_and_abstention();
    test_corrupt_or_unsupported_reference();
    test_byte_isolation();
    puts("PASS: R2.3 integer rarity, abstention, boundaries, corruption, and byte isolation");
    return EXIT_SUCCESS;
}
