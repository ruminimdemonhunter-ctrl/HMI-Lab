#include "l1.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
}

static void test_bounded_slow_adaptation(void) {
    L1Core c; l1_init(&c);
    /* >8 changes in a 600-sample window should increase confirmations by only 1. */
    for (int i = 0; i < L1_SELF_ADAPT_WINDOW; i++) {
        bool high = (i / 4) % 2 == 0;
        l1_step(&c, observe(high ? 5000 : 0, 0, 0));
    }
    assert(c.debounce_extra == 1);
    for (int i = 0; i < L1_SELF_ADAPT_WINDOW; i++) {
        l1_step(&c, observe(0, 0, 0));
    }
    assert(c.debounce_extra <= L1_MAX_EXTRA_CONFIRMATIONS);
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

int main(void) {
    test_initial_and_cpu();
    test_memory_priority_and_invalid();
    test_policy_boundaries();
    test_bounded_slow_adaptation();
    test_psi_parsing();
    test_file_policy();
    puts("PASS: deterministic decisions, priority, hysteresis, invalid telemetry, policy bounds, slow adaptation, PSI parsing");
    return 0;
}
