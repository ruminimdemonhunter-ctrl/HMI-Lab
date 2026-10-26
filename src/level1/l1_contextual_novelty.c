#include "l1_contextual_novelty.h"

#include <stdbool.h>
#include <stddef.h>
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
            sum > reference->accepted_lifetime ||
            (channel != 0U && sum != sums[0])) {
            return false;
        }
        sums[channel] = sum;
    }
    return true;
}

L1ContextualNoveltyResult l1_contextual_novelty_score(
    const L1OperationalMemorySummary *reference,
    L1Observation observation,
    L1PerceptionInputQuality quality,
    uint16_t threshold) {
    L1ContextualNoveltyResult result;
    uint32_t sums[L1_OM_CHANNELS] = {0};
    memset(&result, 0, sizeof(result));
    result.disposition = L1_CONTEXTUAL_NOVELTY_UNKNOWN;
    if (threshold > 1024U || quality != L1_PQ_OK || !observation.valid ||
        observation.cpu_some_bp > 10000U ||
        observation.memory_full_bp > 10000U ||
        observation.io_full_bp > 10000U ||
        !supported_reference(reference, sums)) {
        return result;
    }

    const uint16_t values[L1_OM_CHANNELS] = {
        observation.cpu_some_bp, observation.memory_full_bp,
        observation.io_full_bp
    };
    const uint32_t thresholds[L1_OM_CHANNELS] = {3000U, 100U, 500U};
    for (size_t channel = 0; channel < L1_OM_CHANNELS; ++channel) {
        uint8_t bin = quantize(values[channel], thresholds[channel]);
        uint32_t left = 0;
        uint32_t right = 0;
        for (size_t index = 0; index < L1_OM_BINS; ++index) {
            if (index <= bin) left += reference->bins[channel][index];
            if (index >= bin) right += reference->bins[channel][index];
        }
        uint32_t tail = left < right ? left : right;
        uint32_t deduction = (UINT32_C(2048) * tail) / sums[channel];
        uint16_t score = deduction >= 1024U ? 0U : (uint16_t)(1024U - deduction);
        result.bin[channel] = bin;
        result.channel_score[channel] = score;
        if (score > result.row_score) result.row_score = score;
    }
    result.disposition = result.row_score >= threshold
        ? L1_CONTEXTUAL_NOVELTY_SUSPECTED
        : L1_CONTEXTUAL_NOVELTY_EXPECTED;
    return result;
}
