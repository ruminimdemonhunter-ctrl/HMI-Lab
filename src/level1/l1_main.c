#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool parse_integer(const char *s, unsigned long max, unsigned long *result) {
    if (*s < '0' || *s > '9') return false;
    errno = 0;
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > max) return false;
    *result = n;
    return true;
}

static bool maybe_load(L1Core *core, const char *path) {
    if (!path) return false;
    L1Policy candidate;
    char error[128];
    if (!l1_read_policy(path, &candidate, error, sizeof(error))) {
        fprintf(stderr, "L1: policy ignored (%s); continuing safely\n", error);
        return false;
    }
    if (candidate.generation > core->policy.generation &&
        l1_apply_policy(core, &candidate)) {
        fprintf(stderr, "L1: applied validated behavior memory generation %" PRIu32 "\n",
                core->policy.generation);
        return true;
    }
    return false;
}

static bool monotonic_ms(uint64_t *result) {
    struct timespec now;
    if (result == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0) return false;
    *result = (uint64_t)now.tv_sec * UINT64_C(1000) +
              (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return true;
}

int main(int argc, char **argv) {
    unsigned long samples = 8; /* 0 = continuous */
    unsigned long interval_ms = 1000;
    const char *policy_file = NULL;
    const char *state_file = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--policy") && i + 1 < argc) {
            policy_file = argv[++i];
        } else if (!strcmp(argv[i], "--state") && i + 1 < argc) {
            state_file = argv[++i];
        } else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
            if (!parse_integer(argv[++i], 100000000UL, &samples)) return 2;
        } else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
            if (!parse_integer(argv[++i], 5000, &interval_ms) || interval_ms < 250) return 2;
        } else {
            fprintf(stderr, "Usage: %s [--samples N (0=forever)] [--interval-ms 250..5000] [--policy FILE] [--state FILE]\n", argv[0]);
            return 2;
        }
    }
    L1Core core;
    l1_init(&core);
    if (state_file != NULL) {
        char error[128];
        if (l1_state_load(state_file, &core, error, sizeof(error))) {
            fprintf(stderr, "L1: recovered validated local state generation %" PRIu32 "\n",
                    core.policy.generation);
        } else {
            fprintf(stderr, "L1: state not recovered (%s); using safe defaults\n", error);
        }
    }
    fprintf(stderr, "HMI L1 v0.5: READ-ONLY resource advisory mode; no hardware / OS controls\n");
    for (unsigned long i = 0; samples == 0 || i < samples; ++i) {
        /* A file is only an L2-style memory proposal, not directly writable core memory.
           For live use L2 must publish a complete file via atomic rename. */
        bool state_dirty = i % 5 == 0 && maybe_load(&core, policy_file);
        L1Observation obs;
        bool ok = l1_read_linux_observation(&obs);
        uint64_t observed_at_ms = 0;
        bool clock_ok = monotonic_ms(&observed_at_ms);
        ok = ok && clock_ok;
        if (!clock_ok) obs.valid = false;
        uint8_t previous_cpu_extra = core.cpu_debounce_extra;
        uint8_t previous_io_extra = core.io_debounce_extra;
        L1Action action = l1_step_at(&core, obs, observed_at_ms);
        if (core.cpu_debounce_extra != previous_cpu_extra ||
            core.io_debounce_extra != previous_io_extra) {
            state_dirty = true;
        }
        if (state_dirty && state_file != NULL) {
            char error[128];
            if (!l1_state_save_atomic(state_file, &core, error, sizeof(error))) {
                fprintf(stderr, "L1: state persistence failed (%s); continuing safely\n",
                        error);
            }
        }
        if (ok) {
            printf("sample=%lu cpu_some=%.2f%% mem_full=%.2f%% io_full=%.2f%% action=%s memory_gen=%" PRIu32 " self_cpu_extra=%u self_io_extra=%u\n",
                i + 1, obs.cpu_some_bp / 100.0, obs.memory_full_bp / 100.0,
                obs.io_full_bp / 100.0, l1_action_name(action), core.policy.generation,
                core.cpu_debounce_extra, core.io_debounce_extra);
        } else {
            printf("sample=%lu telemetry=INVALID action=HOLD\n", i + 1);
        }
        fflush(stdout);
        if (samples && i + 1 >= samples) break;
        struct timespec delay = {
            .tv_sec = (time_t)(interval_ms / 1000),
            .tv_nsec = (long)((interval_ms % 1000) * 1000000UL)
        };
        while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
    }
    return 0;
}
