#ifndef HMI_L1_DEMO_H
#define HMI_L1_DEMO_H

#include "l1.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    L1_DEMO_EVENT_NONE = 0,
    L1_DEMO_EVENT_APPLY_POLICY,
    L1_DEMO_EVENT_REJECT_POLICY,
    L1_DEMO_EVENT_ROLLBACK_POLICY
} L1DemoEvent;

typedef struct {
    const char *name;
    uint32_t seed;
    uint32_t rows;
} L1DemoCase;

typedef struct {
    uint64_t monotonic_ms;
    L1Observation observation;
    L1PressureCounters counters;
    L1DemoEvent event;
} L1DemoRow;

size_t l1_demo_case_count(void);
const L1DemoCase *l1_demo_case_at(size_t index);
const L1DemoCase *l1_demo_case_named(const char *name);
bool l1_demo_row(const L1DemoCase *demo, uint32_t index, L1DemoRow *row);
const char *l1_demo_event_name(L1DemoEvent event);
L1Policy l1_demo_valid_policy(void);
L1Policy l1_demo_invalid_policy(void);

#endif
