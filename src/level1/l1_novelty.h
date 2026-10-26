#ifndef HMI_L1_NOVELTY_H
#define HMI_L1_NOVELTY_H

#include "l1_operational_memory.h"

#include <stdint.h>

typedef enum {
    L1_NOVELTY_UNKNOWN = 0,
    L1_NOVELTY_EXPECTED = 1,
    L1_NOVELTY_SUSPECTED = 2
} L1NoveltyDisposition;

typedef struct {
    uint16_t channel_score[L1_OM_CHANNELS];
    uint16_t novelty_score;
    uint8_t bin[L1_OM_CHANNELS];
    L1NoveltyDisposition disposition;
} L1NoveltyResult;

L1NoveltyResult l1_novelty_score(
    const L1OperationalMemorySummary *reference,
    L1Observation observation,
    L1PerceptionInputQuality quality,
    uint16_t threshold);

#endif
