#include "l1_demo.h"

#include <string.h>

enum {
    DEMO_IDLE = 0,
    DEMO_CPU,
    DEMO_MEMORY,
    DEMO_IO,
    DEMO_OVERLAP,
    DEMO_INVALID,
    DEMO_REGRESSION,
    DEMO_RESET,
    DEMO_POLICY,
    DEMO_SHADOW
};

static const L1DemoCase cases[] = {
    {"idle-noise", UINT32_C(0x10203040), 8},
    {"rising-cpu", UINT32_C(0x21314151), 7},
    {"urgent-memory", UINT32_C(0x32425262), 6},
    {"sustained-io", UINT32_C(0x43536373), 7},
    {"overlap-priority", UINT32_C(0x54647484), 12},
    {"invalid-telemetry", UINT32_C(0x65758595), 7},
    {"monotonic-regression", UINT32_C(0x768696a6), 7},
    {"reset-recovery", UINT32_C(0x8797a7b7), 8},
    {"policy-rollback", UINT32_C(0x98a8b8c8), 7},
    {"shadow-prediction", UINT32_C(0xa9b9c9d9), 13}
};

static uint32_t mix(uint32_t seed, uint32_t index) {
    uint32_t value = seed ^ (UINT32_C(0x9e3779b9) * (index + 1U));
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

size_t l1_demo_case_count(void) {
    return sizeof(cases) / sizeof(cases[0]);
}

const L1DemoCase *l1_demo_case_at(size_t index) {
    return index < l1_demo_case_count() ? &cases[index] : NULL;
}

const L1DemoCase *l1_demo_case_named(const char *name) {
    if (name == NULL) return NULL;
    for (size_t i = 0; i < l1_demo_case_count(); ++i)
        if (strcmp(cases[i].name, name) == 0) return &cases[i];
    return NULL;
}

L1Policy l1_demo_valid_policy(void) {
    return (L1Policy){1, 1, 2500, 80, 400, 2};
}

L1Policy l1_demo_invalid_policy(void) {
    return (L1Policy){1, 2, 2500, 10, 400, 2};
}

const char *l1_demo_event_name(L1DemoEvent event) {
    switch (event) {
        case L1_DEMO_EVENT_NONE: return "none";
        case L1_DEMO_EVENT_APPLY_POLICY: return "apply-generation-1";
        case L1_DEMO_EVENT_REJECT_POLICY: return "reject-invalid-generation-2";
        case L1_DEMO_EVENT_ROLLBACK_POLICY: return "rollback-generation-2";
        default: return "unknown";
    }
}

static unsigned case_index(const L1DemoCase *demo) {
    return (unsigned)(demo - cases);
}

bool l1_demo_row(const L1DemoCase *demo, uint32_t index, L1DemoRow *row) {
    if (demo == NULL || row == NULL || demo < cases ||
        demo >= cases + l1_demo_case_count() || index >= demo->rows) {
        return false;
    }
    unsigned id = case_index(demo);
    uint32_t cadence = id == DEMO_SHADOW ? 5000U : 1000U;
    uint64_t time = (uint64_t)index * cadence;
    uint32_t noise = mix(demo->seed, index);
    uint16_t cpu = (uint16_t)(100U + noise % 250U);
    uint16_t memory = (uint16_t)(noise % 10U);
    uint16_t io = (uint16_t)(20U + noise % 40U);
    bool valid = true;
    L1DemoEvent event = L1_DEMO_EVENT_NONE;

    if (id == DEMO_CPU) {
        static const uint16_t values[] = {200, 1200, 3100, 3400, 3700, 3900, 500};
        cpu = values[index];
    } else if (id == DEMO_MEMORY) {
        static const uint16_t values[] = {0, 100, 140, 180, 190, 0};
        memory = values[index];
    } else if (id == DEMO_IO) {
        static const uint16_t values[] = {20, 500, 650, 800, 900, 900, 20};
        io = values[index];
    } else if (id == DEMO_OVERLAP) {
        cpu = index >= 1 ? 3600 : 100;
        io = index >= 4 ? 800 : 20;
        memory = index >= 8 ? 180 : 0;
    } else if (id == DEMO_INVALID) {
        cpu = index < 4 ? 3600 : 3500;
        if (index == 4) valid = false;
    } else if (id == DEMO_REGRESSION) {
        cpu = 3600;
        if (index == 4) time = 2500;
    } else if (id == DEMO_RESET) {
        io = index < 3 || index > 4 ? 800 : 20;
        if (index == 3) valid = false;
    } else if (id == DEMO_POLICY) {
        cpu = 2700;
        if (index == 0) event = L1_DEMO_EVENT_APPLY_POLICY;
        if (index == 3) event = L1_DEMO_EVENT_REJECT_POLICY;
        if (index == 5) event = L1_DEMO_EVENT_ROLLBACK_POLICY;
    } else if (id == DEMO_SHADOW) {
        cpu = index == 0 ? 1200 : 900;
        memory = 10;
        io = 40;
    }

    uint64_t counter_index = index;
    if (id == DEMO_RESET && index >= 4) counter_index = index - 4U;
    L1PressureCounters counters = {
        counter_index * (uint64_t)cadence * 200U,
        counter_index * (uint64_t)cadence * 5U,
        counter_index * (uint64_t)cadence * 10U,
        valid
    };
    L1Observation observation = {cpu, memory, io, valid};
    if (!valid) {
        observation = (L1Observation){0, 0, 0, false};
        counters = (L1PressureCounters){0, 0, 0, false};
    }
    *row = (L1DemoRow){time, observation, counters, event};
    return true;
}
