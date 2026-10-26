#ifndef HMI_L1_CONTEXTUAL_NOVELTY_H
#define HMI_L1_CONTEXTUAL_NOVELTY_H

#include "l1_operational_memory.h"

#include <stdint.h>

typedef enum {
    L1_CONTEXTUAL_NOVELTY_UNKNOWN = 0,
    L1_CONTEXTUAL_NOVELTY_EXPECTED = 1,
    L1_CONTEXTUAL_NOVELTY_SUSPECTED = 2
} L1ContextualNoveltyDisposition;

typedef struct {
    uint16_t channel_score[L1_OM_CHANNELS];
    uint16_t row_score;
    uint8_t bin[L1_OM_CHANNELS];
    L1ContextualNoveltyDisposition disposition;
} L1ContextualNoveltyResult;

L1ContextualNoveltyResult l1_contextual_novelty_score(
    const L1OperationalMemorySummary *reference,
    L1Observation observation,
    L1PerceptionInputQuality quality,
    uint16_t threshold);

#endif
