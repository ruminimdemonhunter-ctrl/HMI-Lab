#include "l1.h"

#include <string.h>

L1Policy l1_default_policy(void) {
    return (L1Policy) {
        .schema_version = 1, .generation = 0,
        .cpu_trigger_bp = 3000,   /* 30 percent CPU some avg10 */
        .memory_trigger_bp = 100, /* 1 percent memory full avg10 */
        .io_trigger_bp = 500,     /* 5 percent IO full avg10 */
        .confirmation_samples = 3
    };
}

void l1_init(L1Core *core) {
    memset(core, 0, sizeof(*core));
    core->policy = l1_default_policy();
    core->action = L1_HOLD;
    core->candidate = L1_HOLD;
}

bool l1_policy_valid(const L1Policy *p) {
    return p != NULL && p->schema_version == 1 && p->generation > 0 &&
        p->cpu_trigger_bp >= 500 && p->cpu_trigger_bp <= 8000 &&
        p->memory_trigger_bp >= 25 && p->memory_trigger_bp <= 3000 &&
        p->io_trigger_bp >= 100 && p->io_trigger_bp <= 5000 &&
        p->confirmation_samples >= 2 && p->confirmation_samples <= 10;
}

static void reset_runtime_state(L1Core *core) {
    core->candidate = L1_HOLD;
    core->candidate_streak = 0;
    core->action = L1_HOLD;
    core->switches_in_window = 0;
    core->debounce_extra = 0;
    core->decisions = 0;
}

bool l1_apply_policy(L1Core *core, const L1Policy *proposal) {
    if (core == NULL || !l1_policy_valid(proposal) ||
        proposal->generation <= core->policy.generation) {
        return false;
    }
    /* Retain one validated snapshot. Only the single event-loop thread writes. */
    core->previous_policy = core->policy;
    core->has_previous_policy = true;
    core->policy = *proposal;
    reset_runtime_state(core); /* fail closed until observations re-confirm */
    return true;
}

bool l1_rollback_policy(L1Core *core, uint32_t new_generation) {
    if (core == NULL || !core->has_previous_policy ||
        new_generation <= core->policy.generation) {
        return false;
    }
    L1Policy rollback = core->previous_policy;
    rollback.generation = new_generation;
    if (!l1_policy_valid(&rollback)) return false;
    core->previous_policy = core->policy;
    core->policy = rollback;
    reset_runtime_state(core);
    return true;
}

static bool above_or_hysteresis(uint16_t observed, uint16_t threshold,
                                uint16_t margin, bool active) {
    uint16_t lower = threshold > margin ? (uint16_t)(threshold - margin) : 0;
    return observed >= (active ? lower : threshold);
}

static L1Action choose_action(const L1Core *core, L1Observation o) {
    if (above_or_hysteresis(o.memory_full_bp,
                            core->policy.memory_trigger_bp, 25,
                            core->action == L1_PROTECT_MEMORY)) {
        return L1_PROTECT_MEMORY;
    }
    if (above_or_hysteresis(o.io_full_bp, core->policy.io_trigger_bp, 100,
                            core->action == L1_DEFER_IO)) {
        return L1_DEFER_IO;
    }
    if (above_or_hysteresis(o.cpu_some_bp, core->policy.cpu_trigger_bp, 300,
                            core->action == L1_PREFER_INTERACTIVE)) {
        return L1_PREFER_INTERACTIVE;
    }
    return L1_HOLD;
}

L1Action l1_step(L1Core *core, L1Observation observation) {
    if (core == NULL) return L1_HOLD;
    if (!observation.valid || observation.cpu_some_bp > 10000 ||
        observation.memory_full_bp > 10000 || observation.io_full_bp > 10000) {
        /* No hardware controls, but HOLD on missing/invalid telemetry. */
        core->action = core->candidate = L1_HOLD;
        core->candidate_streak = 0;
        return L1_HOLD;
    }
    L1Action requested = choose_action(core, observation);
    if (requested == core->action) {
        core->candidate = requested;
        core->candidate_streak = 0;
    } else {
        if (requested != core->candidate) {
            core->candidate = requested;
            core->candidate_streak = 1;
        } else if (core->candidate_streak < UINT16_MAX) {
            core->candidate_streak++;
        }
        if (core->candidate_streak >=
            (uint16_t)(core->policy.confirmation_samples + core->debounce_extra)) {
            core->action = requested;
            core->candidate_streak = 0;
            core->switches_in_window++;
        }
    }

    core->decisions++;
    if (core->decisions % L1_SELF_ADAPT_WINDOW == 0) {
        /* Slow, constrained self-change: only debounce behavior, not safety bounds.
           This heuristic reduces flapping; it is NOT proof of better scheduling. */
        if (core->switches_in_window >= 8 &&
            core->debounce_extra < L1_MAX_EXTRA_CONFIRMATIONS) {
            core->debounce_extra++;
        } else if (core->switches_in_window == 0 && core->debounce_extra > 0) {
            core->debounce_extra--;
        }
        core->switches_in_window = 0;
    }
    return core->action;
}

const char *l1_action_name(L1Action action) {
    switch (action) {
        case L1_HOLD: return "HOLD";
        case L1_PREFER_INTERACTIVE: return "PREFER_INTERACTIVE";
        case L1_PROTECT_MEMORY: return "PROTECT_MEMORY";
        case L1_DEFER_IO: return "DEFER_IO";
        default: return "UNKNOWN";
    }
}
