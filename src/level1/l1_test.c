#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Deliberately independent of assert(3), so -DNDEBUG cannot remove checks. */
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint64_t test_clock_ms;

static uint64_t test_next_ms(void) {
    uint64_t now = test_clock_ms;
    test_clock_ms += 1000;
    return now;
}

#define l1_step(core, observation) \
    l1_step_at((core), (observation), test_next_ms())

static L1Observation observe(uint16_t cpu, uint16_t mem, uint16_t io) {
    return (L1Observation){cpu, mem, io, true};
}

static void test_initial_and_cpu(void) {
    L1Core c; l1_init(&c);
    assert(c.policy.generation == 0 && c.action == L1_HOLD);
    assert(l1_step(&c, observe(0, 0, 0)) == L1_HOLD);
    assert(l1_step(&c, observe(3100, 0, 0)) == L1_HOLD);
    assert(l1_step(&c, observe(3100, 0, 0)) == L1_HOLD);
    assert(l1_step(&c, observe(3100, 0, 0)) == L1_PREFER_INTERACTIVE);
    /* Hysteresis holds CPU preference below the trigger, within its margin. */
    assert(l1_step(&c, observe(2800, 0, 0)) == L1_PREFER_INTERACTIVE);
    assert(l1_step(&c, observe(0, 0, 0)) == L1_PREFER_INTERACTIVE);
    assert(l1_step(&c, observe(0, 0, 0)) == L1_PREFER_INTERACTIVE);
    assert(l1_step(&c, observe(0, 0, 0)) == L1_HOLD);
}

static void test_memory_priority_and_invalid(void) {
    L1Core c; l1_init(&c);
    L1Observation high = observe(9000, 150, 9000);
    for (int i = 0; i < 3; i++) l1_step(&c, high);
    assert(c.action == L1_PROTECT_MEMORY);
    high.valid = false;
    assert(l1_step(&c, high) == L1_HOLD);
    assert(c.candidate_streak == 0);
    high = observe(0, 0, 10001);
    assert(l1_step(&c, high) == L1_HOLD);
}

static void test_policy_boundaries(void) {
    L1Core c; l1_init(&c);
    L1Policy p = l1_default_policy(); p.generation = 1;
    assert(l1_apply_policy(&c, &p));
    assert(c.policy.generation == 1);
    assert(!l1_apply_policy(&c, &p)); /* reject replay */
    p.generation = 2; p.cpu_trigger_bp = 0;
    assert(!l1_apply_policy(&c, &p));
    assert(c.policy.generation == 1); /* unchanged on rejection */
    p.cpu_trigger_bp = 2000;
    assert(l1_apply_policy(&c, &p));
    assert(c.policy.generation == 2 && c.action == L1_HOLD);

    L1Core before = c;
    p.generation = 3; p.confirmation_samples = 1;
    assert(!l1_apply_policy(&c, &p));
    assert(memcmp(&before, &c, sizeof(c)) == 0);
    p.confirmation_samples = 11;
    assert(!l1_apply_policy(&c, &p));
    assert(memcmp(&before, &c, sizeof(c)) == 0);
    p.confirmation_samples = 3; p.schema_version = 2;
    assert(!l1_apply_policy(&c, &p));
    assert(memcmp(&before, &c, sizeof(c)) == 0);
}

static void test_complete_safety_envelope(void) {
    L1Policy p = l1_default_policy();
    p.generation = 1;
    assert(l1_policy_valid(&p));
    assert(!l1_policy_valid(NULL));
    p.generation = 0; assert(!l1_policy_valid(&p)); p.generation = 1;
    p.cpu_trigger_bp = 499; assert(!l1_policy_valid(&p));
    p.cpu_trigger_bp = 8001; assert(!l1_policy_valid(&p)); p.cpu_trigger_bp = 3000;
    p.memory_trigger_bp = 24; assert(!l1_policy_valid(&p));
    p.memory_trigger_bp = 3001; assert(!l1_policy_valid(&p)); p.memory_trigger_bp = 100;
    p.io_trigger_bp = 99; assert(!l1_policy_valid(&p));
    p.io_trigger_bp = 5001; assert(!l1_policy_valid(&p)); p.io_trigger_bp = 500;
    p.confirmation_samples = 1; assert(!l1_policy_valid(&p));
    p.confirmation_samples = 11; assert(!l1_policy_valid(&p));
}

static void test_versioned_rollback(void) {
    L1Core c; l1_init(&c);
    assert(!l1_rollback_policy(&c, 1));
    L1Policy first = l1_default_policy();
    first.generation = 1; first.cpu_trigger_bp = 2000;
    L1Policy second = first;
    second.generation = 2; second.cpu_trigger_bp = 5000;
    assert(l1_apply_policy(&c, &first));
    assert(l1_apply_policy(&c, &second));
    for (int i = 0; i < 3; ++i) l1_step(&c, observe(6000, 0, 0));
    assert(c.action == L1_PREFER_INTERACTIVE);
    assert(!l1_rollback_policy(&c, 2));
    c.cpu_debounce_extra = 2;
    c.io_debounce_extra = 1;
    assert(l1_rollback_policy(&c, 3));
    assert(c.policy.generation == 3 && c.policy.cpu_trigger_bp == 2000);
    assert(c.action == L1_HOLD && c.decisions == 0);
    assert(c.cpu_debounce_extra == 0 && c.io_debounce_extra == 0);
    assert(!l1_apply_policy(&c, &second));
    assert(l1_rollback_policy(&c, 4));
    assert(c.policy.generation == 4 && c.policy.cpu_trigger_bp == 5000);
}

static void test_bounded_slow_adaptation(void) {
    L1Core fast; l1_init(&fast);
    assert(l1_set_adaptation_mode(&fast, L1_ADAPT_LEGACY_GLOBAL));
    for (int i = 0; i < 10000; ++i) {
        bool high = (i / 8) % 2 == 0;
        (void)l1_step_at(&fast, observe(high ? 5000 : 0, 0, 0), 0);
    }
    assert(fast.debounce_extra == 0); /* Call frequency cannot bypass time. */
    (void)l1_step_at(&fast, observe(5000, 0, 0),
                     L1_SELF_ADAPT_MIN_INTERVAL_MS - 1U);
    assert(fast.debounce_extra == 0);
    (void)l1_step_at(&fast, observe(5000, 0, 0),
                     L1_SELF_ADAPT_MIN_INTERVAL_MS);
    assert(fast.debounce_extra == 1);
    uint64_t decisions = fast.decisions;
    assert(l1_step_at(&fast, observe(5000, 0, 0),
                      L1_SELF_ADAPT_MIN_INTERVAL_MS - 1U) == L1_HOLD);
    assert(fast.decisions == decisions); /* Regressing time fails closed. */

    L1Core c; l1_init(&c);
    assert(l1_set_adaptation_mode(&c, L1_ADAPT_LEGACY_GLOBAL));
    for (uint64_t i = 0; i <= 1800; ++i) {
        bool high = (i / 8U) % 2U == 0;
        (void)l1_step_at(&c, observe(high ? 5000 : 0, 0, 0), i * 1000U);
        if (i == 600) assert(c.debounce_extra == 1);
        if (i == 1200) assert(c.debounce_extra == 2);
    }
    assert(c.debounce_extra == L1_MAX_EXTRA_CONFIRMATIONS);
    assert(l1_step_at(&c, (L1Observation){0, 0, 0, false}, 1801000U) == L1_HOLD);
    for (uint64_t i = 1802; i <= 2401; ++i) {
        (void)l1_step_at(&c, observe(0, 0, 0), i * 1000U);
    }
    assert(c.debounce_extra == L1_MAX_EXTRA_CONFIRMATIONS - 1);
}

static void test_adaptation_evaluation_control(void) {
    L1Core c; l1_init(&c);
    assert(c.adaptation_enabled);
    assert(c.adaptation_mode == L1_ADAPT_RISK_AWARE);
    l1_set_adaptation_enabled(&c, false);
    assert(!c.adaptation_enabled);
    for (uint64_t i = 0; i <= 1800; ++i) {
        bool high = (i / 8U) % 2U == 0;
        (void)l1_step_at(&c, observe(high ? 5000 : 0, 0, 0), i * 1000U);
    }
    assert(c.debounce_extra == 0);
    assert(c.valid_samples_in_window == 0);
    assert(!c.adaptation_window_started);
    L1Policy policy = l1_default_policy();
    policy.generation = 1;
    assert(l1_apply_policy(&c, &policy));
    assert(!c.adaptation_enabled); /* Policy memory cannot toggle this control. */

    l1_set_adaptation_enabled(&c, true);
    assert(c.adaptation_enabled);
    assert(c.adaptation_mode == L1_ADAPT_LEGACY_GLOBAL);
    assert(c.debounce_extra == 0);
    L1Core before = c;
    assert(!l1_set_adaptation_mode(&c, (L1AdaptationMode)99));
    assert(memcmp(&before, &c, sizeof(c)) == 0);
    assert(l1_set_adaptation_mode(&c, L1_ADAPT_RISK_AWARE));
    l1_set_adaptation_enabled(NULL, false);
}

static void test_risk_aware_memory_urgency(void) {
    L1Core c; l1_init(&c);
    L1Policy policy = l1_default_policy();
    policy.generation = 1;
    policy.confirmation_samples = 2;
    assert(l1_apply_policy(&c, &policy));
    c.cpu_debounce_extra = 2;
    c.io_debounce_extra = 2;

    assert(l1_step_at(&c, observe(5000, 0, 0), 0) == L1_HOLD);
    assert(c.candidate == L1_PREFER_INTERACTIVE);
    assert(l1_step_at(&c, observe(5000, 200, 900), 1000) == L1_HOLD);
    assert(c.memory_candidate_streak == 1);
    assert(l1_step_at(&c, observe(5000, 200, 900), 2000) ==
           L1_PROTECT_MEMORY);
    assert(c.memory_candidate_streak == 0);
    assert(c.cpu_switches_in_window == 0);
    assert(c.io_switches_in_window == 0);

    L1Core invalid; l1_init(&invalid);
    assert(l1_apply_policy(&invalid, &policy));
    assert(l1_step_at(&invalid, observe(0, 200, 0), 0) == L1_HOLD);
    assert(invalid.memory_candidate_streak == 1);
    assert(l1_step_at(&invalid, (L1Observation){0, 200, 0, false}, 1000) ==
           L1_HOLD);
    assert(invalid.memory_candidate_streak == 0);
    assert(l1_step_at(&invalid, observe(0, 200, 0), 2000) == L1_HOLD);
    assert(l1_step_at(&invalid, observe(0, 200, 0), 1500) == L1_HOLD);
    assert(invalid.memory_candidate_streak == 0);
}

static L1Observation risk_chatter_observation(uint64_t sample) {
    uint64_t phase = sample % 600U;
    if (phase < 300U) {
        return observe((phase / 8U) % 2U == 0U ? 5000 : 0, 0, 0);
    }
    return observe(0, 0, ((phase - 300U) / 8U) % 2U == 0U ? 900 : 0);
}

static void test_risk_aware_bounds_and_time_gate(void) {
    L1Core fast; l1_init(&fast);
    for (uint64_t i = 0; i < 5000; ++i)
        (void)l1_step_at(&fast, risk_chatter_observation(i), 0);
    assert(fast.cpu_debounce_extra == 0 && fast.io_debounce_extra == 0);

    L1Core c; l1_init(&c);
    for (uint64_t i = 0; i <= 1800; ++i) {
        (void)l1_step_at(&c, risk_chatter_observation(i), i * 1000U);
        if (i == 600) {
            assert(c.cpu_debounce_extra == 1);
            assert(c.io_debounce_extra == 1);
        }
        if (i == 1200) {
            assert(c.cpu_debounce_extra == 2);
            assert(c.io_debounce_extra == 2);
        }
    }
    assert(c.cpu_debounce_extra == L1_MAX_EXTRA_CONFIRMATIONS);
    assert(c.io_debounce_extra == L1_MAX_EXTRA_CONFIRMATIONS);
}

static void test_psi_parsing(void) {
    uint16_t bp = 0;
    assert(l1_parse_psi_percent("some avg10=12.34 avg60=1.00 avg300=1.00 total=1\n", "some", &bp));
    assert(bp == 1234);
    assert(!l1_parse_psi_percent("full avg10=12.34\n", "some", &bp));
    assert(!l1_parse_psi_percent("some avg10=nan\n", "some", &bp));
    assert(!l1_parse_psi_percent("some avg10=12.34invalid\n", "some", &bp));
}

static void test_file_policy(void) {
    L1Policy p = {0}; char error[128] = {0};
    assert(l1_read_policy("policy.example", &p, error, sizeof(error)));
    assert(p.generation == 1 && p.confirmation_samples == 3);
    assert(!l1_read_policy("missing.file", &p, error, sizeof(error)));
    assert(!l1_read_policy("policy.invalid.example", &p, error, sizeof(error)));
}

static void assert_invalid_policy_text(const char *text) {
    char path[] = "/tmp/hmi-l1-policy-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    FILE *file = fdopen(descriptor, "w");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
    L1Policy policy = {0};
    char error[128] = {0};
    assert(!l1_read_policy(path, &policy, error, sizeof(error)));
    assert(unlink(path) == 0);
}

static void test_invalid_policy_files(void) {
    const char *prefix =
        "schema_version=1\n"
        "generation=8\n"
        "cpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\n"
        "io_trigger_bp=500\n";
    char duplicate[256];
    assert((size_t)snprintf(duplicate, sizeof(duplicate),
                           "%sconfirmation_samples=3\ngeneration=9\n", prefix)
           < sizeof(duplicate));
    assert_invalid_policy_text(duplicate);
    assert_invalid_policy_text(prefix); /* missing confirmation_samples */
    assert_invalid_policy_text(
        "schema_version=1\ngeneration=eight\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\nconfirmation_samples=3\n");
    assert_invalid_policy_text(
        "schema_version=1\ngeneration=8\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\nconfirmation_samples=3\n"
        "executable_payload=1\n");
}

int main(void) {
    test_initial_and_cpu();
    test_memory_priority_and_invalid();
    test_policy_boundaries();
    test_complete_safety_envelope();
    test_versioned_rollback();
    test_bounded_slow_adaptation();
    test_adaptation_evaluation_control();
    test_risk_aware_memory_urgency();
    test_risk_aware_bounds_and_time_gate();
    test_psi_parsing();
    test_file_policy();
    test_invalid_policy_files();
    puts("PASS: safety fallback, policy bounds/versioning/rollback, slow adaptation, PSI parsing");
    return 0;
}
