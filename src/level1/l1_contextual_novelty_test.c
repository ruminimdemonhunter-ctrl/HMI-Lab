#include "l1_contextual_novelty.h"
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
    L1ContextualNoveltyResult result = l1_contextual_novelty_score(
        &summary, observation, L1_PQ_OK, 900);
    CHECK(result.bin[0] == 0 && result.bin[1] == 2 && result.bin[2] == 7);
    CHECK(result.channel_score[0] == 968);
    CHECK(result.channel_score[1] == 683);
    CHECK(result.channel_score[2] == 569);
    CHECK(result.row_score == 968);
    CHECK(result.disposition == L1_CONTEXTUAL_NOVELTY_SUSPECTED);
    result = l1_contextual_novelty_score(&summary, observation, L1_PQ_OK, 1000);
    CHECK(result.disposition == L1_CONTEXTUAL_NOVELTY_EXPECTED);

    memset(summary.bins, 0, sizeof(summary.bins));
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        summary.bins[channel][4] = 256;
    }
    result = l1_contextual_novelty_score(
        &summary, (L1Observation){0, 0, 0, true}, L1_PQ_OK, 1024);
    CHECK(result.row_score == 1024);
    CHECK(result.disposition == L1_CONTEXTUAL_NOVELTY_SUSPECTED);
}

static void test_all_bins_and_abstention(void) {
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
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        L1ContextualNoveltyResult result = l1_contextual_novelty_score(
            &summary, (L1Observation){values[index][0], values[index][1],
                                     values[index][2], true}, L1_PQ_OK, 1024);
        CHECK(result.disposition != L1_CONTEXTUAL_NOVELTY_UNKNOWN);
        CHECK(memcmp(result.bin, expected[index], sizeof(result.bin)) == 0);
    }
    const L1PerceptionInputQuality bad[] = {
        L1_PQ_ABSENT, L1_PQ_INVALID, L1_PQ_STALE, L1_PQ_REGRESSED,
        L1_PQ_RESET, (L1PerceptionInputQuality)-1
    };
    for (size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        CHECK(l1_contextual_novelty_score(&summary,
              (L1Observation){0,0,0,true}, bad[index], 512).disposition ==
              L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    }
    CHECK(l1_contextual_novelty_score(NULL, (L1Observation){0,0,0,true},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){0,0,0,false},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){10001,0,0,true},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){0,0,0,true},
          L1_PQ_OK, 1025).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
}

static void test_corrupt_reference_and_isolation(void) {
    L1OperationalMemorySummary summary = reference();
    summary.status = L1_OM_WARMING_UP;
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){0,0,0,true},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    summary = reference(); summary.bins[1][0]++;
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){0,0,0,true},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);
    summary = reference(); summary.accepted_lifetime = 10;
    CHECK(l1_contextual_novelty_score(&summary, (L1Observation){0,0,0,true},
          L1_PQ_OK, 512).disposition == L1_CONTEXTUAL_NOVELTY_UNKNOWN);

    L1OperationalMemory memory;
    l1_operational_memory_init(&memory);
    for (size_t index = 0; index < 300; ++index) {
        (void)l1_operational_memory_step(&memory,
            (L1Observation){1200,20,100,true}, L1_PQ_OK,
            (uint64_t)(index / 75U) * L1_OM_WINDOW_MS +
            (uint64_t)(index % 75U) * L1_OM_SAMPLE_INTERVAL_MS);
    }
    summary = l1_operational_memory_summary(&memory);
    L1OperationalMemory before_memory = memory;
    L1OperationalMemorySummary before_summary = summary;
    L1Core enabled, disabled;
    L1Shadow enabled_shadow, disabled_shadow;
    l1_init(&enabled); l1_init(&disabled);
    l1_shadow_init(&enabled_shadow, l1_default_forecast_thresholds());
    l1_shadow_init(&disabled_shadow, l1_default_forecast_thresholds());
    for (size_t index = 0; index < 32; ++index) {
        L1Observation observation = {(uint16_t)(index * 503U % 10001U),
            (uint16_t)(index * 47U % 10001U),
            (uint16_t)(index * 211U % 10001U), true};
        (void)l1_contextual_novelty_score(&summary, observation, L1_PQ_OK, 768);
        CHECK(l1_step_at(&enabled, observation, index * 1000U) ==
              l1_step_at(&disabled, observation, index * 1000U));
        CHECK(memcmp(&enabled, &disabled, sizeof(enabled)) == 0);
        L1PressureCounters counters = {index * 100U, index * 10U, index * 20U, true};
        (void)l1_shadow_step_at(&enabled_shadow, observation, counters, index * 1000U);
        (void)l1_shadow_step_at(&disabled_shadow, observation, counters, index * 1000U);
        CHECK(memcmp(&enabled_shadow, &disabled_shadow, sizeof(enabled_shadow)) == 0);
    }
    CHECK(memcmp(&memory, &before_memory, sizeof(memory)) == 0);
    CHECK(memcmp(&summary, &before_summary, sizeof(summary)) == 0);
    CHECK(sizeof(L1Core) == 104U && sizeof(L1Shadow) == 208U);
    CHECK(sizeof(L1OperationalMemory) == 112U);
}

int main(void) {
    test_integer_equation_and_threshold();
    test_all_bins_and_abstention();
    test_corrupt_reference_and_isolation();
    puts("PASS: R2.3b two-sided tail surprise, abstention, corruption, and byte isolation");
    return EXIT_SUCCESS;
}
