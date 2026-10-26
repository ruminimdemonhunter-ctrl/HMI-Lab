#ifndef HMI_L1_PERCEPTION_H
#define HMI_L1_PERCEPTION_H

#include "l1.h"

#include <stdint.h>

typedef enum {
    L1_PQ_OK = 0,
    L1_PQ_ABSENT = 1,
    L1_PQ_INVALID = 2,
    L1_PQ_STALE = 3,
    L1_PQ_REGRESSED = 4,
    L1_PQ_RESET = 5
} L1PerceptionInputQuality;

typedef enum {
    L1_PS_UNKNOWN = 0,
    L1_PS_QUIET = 1,
    L1_PS_CPU_PRESSURE = 2,
    L1_PS_MEMORY_PRESSURE = 3,
    L1_PS_IO_PRESSURE = 4,
    L1_PS_MIXED_PRESSURE = 5,
    L1_PS_UNRELIABLE = 6
} L1PerceptionState;

typedef struct {
    L1PerceptionState state;
    L1PerceptionInputQuality quality;
    uint8_t pressure_mask;
} L1PerceptionResult;

L1PerceptionResult l1_perception_classify(
    L1Observation observation, L1PerceptionInputQuality quality);

#endif
