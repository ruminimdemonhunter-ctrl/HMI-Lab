#define _POSIX_C_SOURCE 200809L
#include "l1.h"
#include "l1_proposal.h"
#include "l1_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const char canonical[] =
    "HMI_L1_POLICY_PROPOSAL_V1\n"
    "policy_schema_version=1\n"
    "base_generation=0\n"
    "generation=1\n"
    "cpu_trigger_bp=3000\n"
    "memory_trigger_bp=100\n"
    "io_trigger_bp=500\n"
    "confirmation_samples=2\n"
    "END\n";

typedef struct {
    uint64_t time_ms;
    L1Observation observation;
    L1Action expected;
} IntegratedStep;

typedef struct {
    const uint8_t *bytes;
    size_t length;
} VerifierContext;

static bool exact_test_verifier(const uint8_t *bytes, size_t length,
                                void *context) {
    const VerifierContext *expected = context;
    return expected != NULL && length == expected->length &&
        memcmp(bytes, expected->bytes, length) == 0;
}

static void test_priority_fallback_and_observer_equivalence(void) {
    static const IntegratedStep steps[] = {
        {0,    {5000, 0,   0,   true},  L1_HOLD},
        {1000, {5000, 0,   0,   true},  L1_PREFER_INTERACTIVE},
        {2000, {5000, 200, 900, true},  L1_PREFER_INTERACTIVE},
        {3000, {5000, 200, 900, true},  L1_PROTECT_MEMORY},
        {4000, {5000, 200, 900, false}, L1_HOLD},
        {5000, {0,    0,   900, true},  L1_HOLD},
        {4500, {0,    0,   900, true},  L1_HOLD},
        {6000, {0,    0,   900, true},  L1_HOLD},
        {7000, {0,    0,   900, true},  L1_DEFER_IO},
        {8000, {0,    0,   0,   true},  L1_DEFER_IO},
        {9000, {0,    0,   0,   true},  L1_HOLD}
    };
    L1Core observed, reference;
    L1Shadow shadow;
    l1_init(&observed);
    l1_init(&reference);
    L1Policy policy = l1_default_policy();
    policy.generation = 1;
    policy.confirmation_samples = 2;
    CHECK(l1_apply_policy(&observed, &policy));
    CHECK(l1_apply_policy(&reference, &policy));
    l1_shadow_init(&shadow, l1_default_forecast_thresholds());

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        L1PressureCounters counters = {
            .cpu_some_total_us = (uint64_t)i * 1000U,
            .memory_full_total_us = (uint64_t)i * 100U,
            .io_full_total_us = (uint64_t)i * 500U,
            .valid = steps[i].observation.valid
        };
        L1Action action = l1_step_at(&observed, steps[i].observation,
                                     steps[i].time_ms);
        (void)l1_shadow_step_at(&shadow, steps[i].observation, counters,
                                steps[i].time_ms);
        L1Action reference_action = l1_step_at(
            &reference, steps[i].observation, steps[i].time_ms);
        CHECK(action == steps[i].expected);
        CHECK(action == reference_action);
        CHECK(memcmp(&observed, &reference, sizeof(observed)) == 0);
    }
    CHECK(observed.action == L1_HOLD);
}

static void test_policy_proposal_and_rollback_isolation(void) {
    L1Core current;
    l1_init(&current);
    L1Core before = current;
    L1Policy invalid = l1_default_policy();
    invalid.generation = 1;
    invalid.memory_trigger_bp = 24;
    CHECK(!l1_apply_policy(&current, &invalid));
    CHECK(memcmp(&current, &before, sizeof(current)) == 0);

    L1Core denied;
    memset(&denied, 0xa5, sizeof(denied));
    L1Core denied_before = denied;
    L1ProposalStatus status = L1_PROPOSAL_OK;
    CHECK(!l1_proposal_test_apply_clone(
        &current, (const uint8_t *)canonical, sizeof(canonical) - 1U,
        NULL, NULL, &denied, &status));
    CHECK(status == L1_PROPOSAL_AUTH_UNAVAILABLE);
    CHECK(memcmp(&current, &before, sizeof(current)) == 0);
    CHECK(memcmp(&denied, &denied_before, sizeof(denied)) == 0);

    VerifierContext verifier = {
        (const uint8_t *)canonical, sizeof(canonical) - 1U
    };
    L1Core accepted;
    CHECK(l1_proposal_test_apply_clone(
        &current, (const uint8_t *)canonical, sizeof(canonical) - 1U,
        exact_test_verifier, &verifier, &accepted, &status));
    CHECK(memcmp(&current, &before, sizeof(current)) == 0);
    CHECK(accepted.policy.generation == 1);
    CHECK(accepted.action == L1_HOLD && accepted.decisions == 0);

    L1Core stale_result;
    memset(&stale_result, 0x5a, sizeof(stale_result));
    L1Core stale_before = stale_result;
    L1Core accepted_before = accepted;
    CHECK(!l1_proposal_test_apply_clone(
        &accepted, (const uint8_t *)canonical, sizeof(canonical) - 1U,
        exact_test_verifier, &verifier, &stale_result, &status));
    CHECK(status == L1_PROPOSAL_BASE_MISMATCH);
    CHECK(memcmp(&accepted, &accepted_before, sizeof(accepted)) == 0);
    CHECK(memcmp(&stale_result, &stale_before, sizeof(stale_result)) == 0);

    CHECK(l1_rollback_policy(&accepted, 2));
    CHECK(accepted.policy.generation == 2);
    CHECK(accepted.policy.cpu_trigger_bp == 3000);
    CHECK(accepted.policy.memory_trigger_bp == 100);
    CHECK(accepted.policy.io_trigger_bp == 500);
    CHECK(accepted.action == L1_HOLD && accepted.decisions == 0);
}

static void corrupt_state(const char *path) {
    FILE *file = fopen(path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, 24, SEEK_SET) == 0);
    int value = fgetc(file);
    CHECK(value != EOF);
    CHECK(fseek(file, 24, SEEK_SET) == 0);
    CHECK(fputc(value ^ 0x5a, file) != EOF);
    CHECK(fclose(file) == 0);
}

static void test_corrupted_state_safe_recovery(void) {
    char directory[] = "/tmp/hmi-l1-rc0-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    CHECK((size_t)snprintf(path, sizeof(path), "%s/state.bin", directory) <
          sizeof(path));
    char error[128] = {0};
    L1Core stored;
    l1_init(&stored);
    L1Policy policy = l1_default_policy();
    policy.generation = 1;
    CHECK(l1_apply_policy(&stored, &policy));
    CHECK(l1_state_save_atomic(path, &stored, error, sizeof(error)));
    corrupt_state(path);

    L1Core recovered;
    memset(&recovered, 0xa5, sizeof(recovered));
    CHECK(!l1_state_load(path, &recovered, error, sizeof(error)));
    L1Core defaults;
    l1_init(&defaults);
    CHECK(memcmp(&recovered, &defaults, sizeof(recovered)) == 0);
    CHECK(recovered.action == L1_HOLD);

    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
}

int main(void) {
    CHECK(sizeof(L1Core) == 104);
    CHECK(sizeof(L1Shadow) == 208);
    test_priority_fallback_and_observer_equivalence();
    test_policy_proposal_and_rollback_isolation();
    test_corrupted_state_safe_recovery();
    puts("PASS: RC0 priority, HOLD fallback, observer equivalence, proposal isolation, rollback, and corrupt-state recovery");
    return EXIT_SUCCESS;
}
