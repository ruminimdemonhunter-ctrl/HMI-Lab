#include "l1_novelty.h"

#include <stdbool.h>
#include <string.h>

static uint8_t quantize(uint16_t value, uint32_t threshold) {
    uint32_t quarter = (threshold + 3U) / 4U;
    uint32_t half = (threshold + 1U) / 2U;
    uint32_t wide = value;
    if (wide == 0U) return 0;
    if (wide < quarter) return 1;
    if (wide < half) return 2;
    if (wide < threshold) return 3;
    if (wide < threshold * 2U) return 4;
    if (wide < threshold * 4U) return 5;
    if (wide < threshold * 8U) return 6;
    return 7;
}

static bool supported_reference(const L1OperationalMemorySummary *reference,
                                uint32_t sums[L1_OM_CHANNELS]) {
    if (reference == NULL || reference->status != L1_OM_SUPPORTED ||
        reference->accepted_lifetime < L1_OM_MIN_SUPPORT_SAMPLES ||
        reference->windows_seen < L1_OM_MIN_SUPPORT_WINDOWS) {
        return false;
    }
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        uint32_t sum = 0;
        for (size_t bin = 0; bin < L1_OM_BINS; ++bin) {
            sum += reference->bins[channel][bin];
        }
        if (sum == 0U || sum > L1_OM_DECAY_SUM ||
            sum > reference->accepted_lifetime) {
            return false;
        }
        sums[channel] = sum;
    }
    return true;
}

L1NoveltyResult l1_novelty_score(
    const L1OperationalMemorySummary *reference,
    L1Observation observation,
    L1PerceptionInputQuality quality,
    uint16_t threshold) {
    L1NoveltyResult result;
    memset(&result, 0, sizeof(result));
    result.disposition = L1_NOVELTY_UNKNOWN;
    uint32_t sums[L1_OM_CHANNELS];
    if (threshold > 1024U || quality != L1_PQ_OK || !observation.valid ||
        observation.cpu_some_bp > 10000U ||
        observation.memory_full_bp > 10000U ||
        observation.io_full_bp > 10000U ||
        !supported_reference(reference, sums)) {
        return result;
    }

    const uint16_t values[L1_OM_CHANNELS] = {
        observation.cpu_some_bp,
        observation.memory_full_bp,
        observation.io_full_bp
    };
    const uint32_t boundaries[L1_OM_CHANNELS] = {3000U, 100U, 500U};
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        uint8_t bin = quantize(values[channel], boundaries[channel]);
        uint32_t numerator = UINT32_C(1024) *
            (sums[channel] + L1_OM_BINS -
             ((uint32_t)reference->bins[channel][bin] + 1U));
        uint16_t score = (uint16_t)(numerator /
            (sums[channel] + L1_OM_BINS));
        result.bin[channel] = bin;
        result.channel_score[channel] = score;
        if (score > result.novelty_score) {
            result.novelty_score = score;
        }
    }
    result.disposition = result.novelty_score >= threshold
        ? L1_NOVELTY_SUSPECTED : L1_NOVELTY_EXPECTED;
    return result;
}
