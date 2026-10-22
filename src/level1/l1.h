#ifndef HMI_L1_H
#define HMI_L1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All pressure values are basis points (0..10000 = 0..100 percent). */
typedef struct {
    uint16_t cpu_some_bp;
    uint16_t memory_full_bp;
    uint16_t io_full_bp;
    bool valid;
} L1Observation;

typedef enum {
    L1_HOLD = 0,
    L1_PREFER_INTERACTIVE = 1,
    L1_PROTECT_MEMORY = 2,
    L1_DEFER_IO = 3
} L1Action;

typedef enum {
    L1_ADAPT_DISABLED = 0,
    L1_ADAPT_LEGACY_GLOBAL = 1,
    L1_ADAPT_RISK_AWARE = 2
} L1AdaptationMode;

/* L2 may propose only these behavior parameters (never executable code). */
typedef struct {
    uint32_t schema_version;
    uint32_t generation;
    uint16_t cpu_trigger_bp;
    uint16_t memory_trigger_bp;
    uint16_t io_trigger_bp;
    uint8_t confirmation_samples;
} L1Policy;

typedef struct {
    L1Policy policy;                /* behavior-defining memory */
    L1Policy previous_policy;       /* last validated rollback snapshot */
    L1Action action;              /* active recommendation */
    L1Action candidate;           /* unconfirmed recommendation */
    uint16_t candidate_streak;
    uint16_t memory_candidate_streak;
    uint8_t debounce_extra;       /* legacy V0.4 global value: 0..2 */
    uint8_t cpu_debounce_extra;   /* V0.5 independently bounded: 0..2 */
    uint8_t io_debounce_extra;    /* V0.5 independently bounded: 0..2 */
    uint32_t switches_in_window;
    uint32_t cpu_switches_in_window;
    uint32_t io_switches_in_window;
    uint32_t valid_samples_in_window;
    uint64_t decisions;
    uint64_t adaptation_window_started_ms;
    uint64_t last_monotonic_ms;
    bool has_previous_policy;
    bool adaptation_enabled;      /* compatibility mirror of mode != disabled */
    L1AdaptationMode adaptation_mode;
    bool adaptation_window_started;
    bool monotonic_initialized;
} L1Core;

enum {
    L1_SELF_ADAPT_MIN_SAMPLES = 600,
    L1_SELF_ADAPT_MIN_INTERVAL_MS = 600000,
    L1_MAX_EXTRA_CONFIRMATIONS = 2
};

L1Policy l1_default_policy(void);
void l1_init(L1Core *core);
void l1_set_adaptation_enabled(L1Core *core, bool enabled);
bool l1_set_adaptation_mode(L1Core *core, L1AdaptationMode mode);
bool l1_policy_valid(const L1Policy *policy);
bool l1_apply_policy(L1Core *core, const L1Policy *proposal);
bool l1_rollback_policy(L1Core *core, uint32_t new_generation);
L1Action l1_step_at(L1Core *core, L1Observation observation,
                    uint64_t monotonic_ms);
const char *l1_action_name(L1Action action);

/* A deliberately strict text-file format for future L2 policy proposals. */
bool l1_read_policy(const char *path, L1Policy *out, char *error, size_t error_size);

/* Explicit local persistence; CRC32 detects corruption but is not authentication. */
bool l1_state_save_atomic(const char *path, const L1Core *core,
                          char *error, size_t error_size);
bool l1_state_load(const char *path, L1Core *core,
                   char *error, size_t error_size);

/* Linux PSI adapter is OUTSIDE the deterministic L1 decision core. */
bool l1_parse_psi_percent(const char *line, const char *category, uint16_t *bp);
bool l1_read_linux_observation(L1Observation *out);

#endif
