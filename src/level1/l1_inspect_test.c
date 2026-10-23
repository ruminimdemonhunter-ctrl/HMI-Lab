#include "l1_demo.h"
#include "l1_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const L1Action expected[][13] = {
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD},
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_PREFER_INTERACTIVE,
     L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE},
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_PROTECT_MEMORY, L1_PROTECT_MEMORY,
     L1_PROTECT_MEMORY},
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_DEFER_IO, L1_DEFER_IO, L1_DEFER_IO,
     L1_DEFER_IO},
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_PREFER_INTERACTIVE,
     L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE, L1_DEFER_IO, L1_DEFER_IO,
     L1_DEFER_IO, L1_DEFER_IO, L1_PROTECT_MEMORY, L1_PROTECT_MEMORY},
    {L1_HOLD, L1_HOLD, L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE,
     L1_HOLD, L1_HOLD, L1_HOLD},
    {L1_HOLD, L1_HOLD, L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE,
     L1_HOLD, L1_HOLD, L1_HOLD},
    {L1_HOLD, L1_HOLD, L1_DEFER_IO, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD,
     L1_DEFER_IO},
    {L1_HOLD, L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE,
     L1_PREFER_INTERACTIVE, L1_PREFER_INTERACTIVE, L1_HOLD, L1_HOLD},
    {L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD,
     L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD, L1_HOLD}
};

static bool apply_event(L1Core *core, L1DemoEvent event) {
    if (event == L1_DEMO_EVENT_NONE) return true;
    if (event == L1_DEMO_EVENT_APPLY_POLICY) {
        L1Policy policy = l1_demo_valid_policy();
        return l1_apply_policy(core, &policy);
    }
    if (event == L1_DEMO_EVENT_REJECT_POLICY) {
        L1Policy policy = l1_demo_invalid_policy();
        return !l1_apply_policy(core, &policy);
    }
    return l1_rollback_policy(core, 2);
}

int main(void) {
    CHECK(l1_demo_case_count() == 10);
    uint64_t total_rows = 0;
    unsigned feedback = 0;
    for (size_t c = 0; c < l1_demo_case_count(); ++c) {
        const L1DemoCase *demo = l1_demo_case_at(c);
        CHECK(demo != NULL && l1_demo_case_named(demo->name) == demo);
        L1Core observed, reference;
        L1Shadow shadow;
        l1_init(&observed);
        l1_init(&reference);
        l1_shadow_init(&shadow, l1_default_forecast_thresholds());
        for (uint32_t i = 0; i < demo->rows; ++i) {
            L1DemoRow row;
            CHECK(l1_demo_row(demo, i, &row));
            CHECK(apply_event(&observed, row.event));
            CHECK(apply_event(&reference, row.event));
            L1Action action = l1_step_at(&observed, row.observation,
                                         row.monotonic_ms);
            (void)l1_shadow_step_at(&shadow, row.observation, row.counters,
                                    row.monotonic_ms);
            L1Action reference_action = l1_step_at(
                &reference, row.observation, row.monotonic_ms);
            CHECK(action == expected[c][i]);
            CHECK(action == reference_action);
            CHECK(memcmp(&observed, &reference, sizeof(observed)) == 0);
            total_rows++;
        }
        feedback += shadow.feedback_windows;
        if (strcmp(demo->name, "policy-rollback") == 0) {
            CHECK(observed.policy.generation == 2);
            CHECK(observed.policy.cpu_trigger_bp == 3000);
            CHECK(observed.policy.memory_trigger_bp == 100);
            CHECK(observed.policy.io_trigger_bp == 500);
        }
    }
    CHECK(total_rows == 82);
    CHECK(feedback == 1);
    CHECK(l1_demo_case_at(l1_demo_case_count()) == NULL);
    CHECK(l1_demo_case_named("not-a-case") == NULL);
    puts("PASS: seeded demos, expected actions, rollback, shadow feedback, and observer equivalence");
    return 0;
}
