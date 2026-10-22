#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    assert(l1_rollback_policy(&c, 3));
    assert(c.policy.generation == 3 && c.policy.cpu_trigger_bp == 2000);
    assert(c.action == L1_HOLD && c.decisions == 0);
    assert(!l1_apply_policy(&c, &second));
    assert(l1_rollback_policy(&c, 4));
    assert(c.policy.generation == 4 && c.policy.cpu_trigger_bp == 5000);
}

static void test_bounded_slow_adaptation(void) {
    L1Core c; l1_init(&c);
    /* Wide blocks keep switching even at the maximum extra confirmation count. */
    for (int window = 0; window < 3; ++window) {
        for (int i = 0; i < L1_SELF_ADAPT_WINDOW; ++i) {
            bool high = (i / 8) % 2 == 0;
            l1_step(&c, observe(high ? 5000 : 0, 0, 0));
        }
        uint8_t expected = (uint8_t)(window + 1);
        if (expected > L1_MAX_EXTRA_CONFIRMATIONS) {
            expected = L1_MAX_EXTRA_CONFIRMATIONS;
        }
        assert(c.debounce_extra == expected);
    }
    assert(l1_step(&c, (L1Observation){0, 0, 0, false}) == L1_HOLD);
    for (int i = 0; i < L1_SELF_ADAPT_WINDOW; i++) {
        l1_step(&c, observe(0, 0, 0));
    }
    assert(c.debounce_extra == L1_MAX_EXTRA_CONFIRMATIONS - 1);
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
    test_psi_parsing();
    test_file_policy();
    test_invalid_policy_files();
    puts("PASS: safety fallback, policy bounds/versioning/rollback, slow adaptation, PSI parsing");
    return 0;
}
