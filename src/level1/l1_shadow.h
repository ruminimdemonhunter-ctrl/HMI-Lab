#ifndef HMI_L1_SHADOW_H
#define HMI_L1_SHADOW_H

#include "l1.h"

enum {
    L1_FORECAST_CPU = 0,
    L1_FORECAST_MEMORY = 1,
    L1_FORECAST_IO = 2,
    L1_FORECAST_CLASS_COUNT = 3,
    L1_FORECAST_HORIZON_MS = 60000,
    L1_FORECAST_LATEST_MS = 65000,
    L1_FORECAST_MAX_GAP_MS = 5000,
    L1_FORECAST_MIN_WINDOWS = 8,
    L1_FORECAST_MAX_OFFSET = 2
};

typedef struct {
    uint16_t cpu_bp;
    uint16_t memory_bp;
    uint16_t io_bp;
} L1ForecastThresholds;

typedef struct {
    uint8_t positive_mask;
    uint16_t effective_threshold_bp[L1_FORECAST_CLASS_COUNT];
} L1Prediction;

typedef struct {
    uint8_t positive_mask;
    uint16_t pressure_bp[L1_FORECAST_CLASS_COUNT];
    bool available;
} L1Feedback;

typedef struct {
    bool prediction_issued;
    bool feedback_ready;
    L1Prediction prediction;
    L1Feedback feedback;
} L1ShadowStep;

typedef struct {
    L1ForecastThresholds base_thresholds;
    int8_t offset[L1_FORECAST_CLASS_COUNT];
    uint16_t false_alarms[L1_FORECAST_CLASS_COUNT];
    uint16_t misses[L1_FORECAST_CLASS_COUNT];
    uint16_t evaluated_windows[L1_FORECAST_CLASS_COUNT];
    uint32_t valid_samples[L1_FORECAST_CLASS_COUNT];
    uint64_t gate_started_ms[L1_FORECAST_CLASS_COUNT];
    bool gate_started[L1_FORECAST_CLASS_COUNT];

    L1Prediction pending_prediction;
    L1PressureCounters pending_counters;
    uint64_t pending_started_ms;
    bool pending;

    L1PressureCounters last_counters;
    uint64_t last_monotonic_ms;
    bool monotonic_initialized;

    uint32_t predictions_issued;
    uint32_t feedback_windows;
    uint32_t discarded_invalid;
    uint32_t discarded_gap_or_late;
    uint32_t discarded_reset;
    uint32_t reset_count;
    uint32_t updates[L1_FORECAST_CLASS_COUNT];
} L1Shadow;

L1ForecastThresholds l1_default_forecast_thresholds(void);
void l1_shadow_init(L1Shadow *shadow, L1ForecastThresholds thresholds);
void l1_shadow_reset(L1Shadow *shadow);
bool l1_forecast_thresholds_valid(L1ForecastThresholds thresholds);
bool l1_fixed_prediction(L1Observation observation,
                         L1ForecastThresholds thresholds,
                         L1Prediction *prediction);
L1ShadowStep l1_shadow_step_at(L1Shadow *shadow, L1Observation observation,
                               L1PressureCounters counters,
                               uint64_t monotonic_ms);

#endif
