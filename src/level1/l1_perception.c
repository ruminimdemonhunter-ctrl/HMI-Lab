#include "l1_perception.h"

enum {
    L1_PERCEPTION_CPU_TRIGGER_BP = 3000,
    L1_PERCEPTION_MEMORY_TRIGGER_BP = 100,
    L1_PERCEPTION_IO_TRIGGER_BP = 500,
    L1_PERCEPTION_MAX_BP = 10000,
    L1_PERCEPTION_CPU_MASK = 0x01,
    L1_PERCEPTION_MEMORY_MASK = 0x02,
    L1_PERCEPTION_IO_MASK = 0x04
};

static L1PerceptionResult result(L1PerceptionState state,
                                 L1PerceptionInputQuality quality,
                                 uint8_t pressure_mask) {
    L1PerceptionResult classified = {0};
    classified.state = state;
    classified.quality = quality;
    classified.pressure_mask = pressure_mask;
    return classified;
}

L1PerceptionResult l1_perception_classify(
    L1Observation observation, L1PerceptionInputQuality quality) {
    switch (quality) {
        case L1_PQ_OK:
            break;
        case L1_PQ_ABSENT:
            return result(L1_PS_UNKNOWN, L1_PQ_ABSENT, 0);
        case L1_PQ_INVALID:
        case L1_PQ_STALE:
        case L1_PQ_REGRESSED:
        case L1_PQ_RESET:
            return result(L1_PS_UNRELIABLE, quality, 0);
        default:
            return result(L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    }

    if (!observation.valid || observation.cpu_some_bp > L1_PERCEPTION_MAX_BP ||
        observation.memory_full_bp > L1_PERCEPTION_MAX_BP ||
        observation.io_full_bp > L1_PERCEPTION_MAX_BP) {
        return result(L1_PS_UNRELIABLE, L1_PQ_INVALID, 0);
    }

    uint8_t mask = 0;
    if (observation.cpu_some_bp >= L1_PERCEPTION_CPU_TRIGGER_BP) {
        mask |= L1_PERCEPTION_CPU_MASK;
    }
    if (observation.memory_full_bp >= L1_PERCEPTION_MEMORY_TRIGGER_BP) {
        mask |= L1_PERCEPTION_MEMORY_MASK;
    }
    if (observation.io_full_bp >= L1_PERCEPTION_IO_TRIGGER_BP) {
        mask |= L1_PERCEPTION_IO_MASK;
    }

    switch (mask) {
        case 0:
            return result(L1_PS_QUIET, L1_PQ_OK, mask);
        case L1_PERCEPTION_CPU_MASK:
            return result(L1_PS_CPU_PRESSURE, L1_PQ_OK, mask);
        case L1_PERCEPTION_MEMORY_MASK:
            return result(L1_PS_MEMORY_PRESSURE, L1_PQ_OK, mask);
        case L1_PERCEPTION_IO_MASK:
            return result(L1_PS_IO_PRESSURE, L1_PQ_OK, mask);
        default:
            return result(L1_PS_MIXED_PRESSURE, L1_PQ_OK, mask);
    }
}
