#define _POSIX_C_SOURCE 200809L
#include "l1_proposal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    "confirmation_samples=3\n"
    "END\n";

static void expect_parse_failure(const void *bytes, size_t length,
                                 L1ProposalStatus expected) {
    L1Proposal proposal;
    memset(&proposal, 0xa5, sizeof(proposal));
    L1Proposal before = proposal;
    L1ProposalStatus status = L1_PROPOSAL_OK;
    CHECK(!l1_proposal_parse(bytes, length, &proposal, &status));
    CHECK(status == expected);
    CHECK(memcmp(&proposal, &before, sizeof(proposal)) == 0);
}

static size_t make_proposal(char *buffer, size_t size, uint32_t schema,
                            uint32_t base, uint32_t generation,
                            unsigned cpu, unsigned memory, unsigned io,
                            unsigned confirmations) {
    int count = snprintf(
        buffer, size,
        "HMI_L1_POLICY_PROPOSAL_V1\n"
        "policy_schema_version=%u\n"
        "base_generation=%u\n"
        "generation=%u\n"
        "cpu_trigger_bp=%u\n"
        "memory_trigger_bp=%u\n"
        "io_trigger_bp=%u\n"
        "confirmation_samples=%u\n"
        "END\n",
        schema, base, generation, cpu, memory, io, confirmations);
    CHECK(count > 0 && (size_t)count < size);
    return (size_t)count;
}

static void test_canonical_and_bounds(void) {
    L1Proposal proposal;
    L1ProposalStatus status;
    CHECK(l1_proposal_parse((const uint8_t *)canonical,
                            sizeof(canonical) - 1U, &proposal, &status));
    CHECK(status == L1_PROPOSAL_OK);
    CHECK(proposal.base_generation == 0);
    CHECK(proposal.policy.generation == 1);
    CHECK(proposal.policy.cpu_trigger_bp == 3000);
    CHECK(proposal.policy.memory_trigger_bp == 100);
    CHECK(proposal.policy.io_trigger_bp == 500);
    CHECK(proposal.policy.confirmation_samples == 3);

    char text[512];
    size_t length = make_proposal(text, sizeof(text), 1, 7, 8,
                                  500, 25, 100, 2);
    CHECK(l1_proposal_parse((const uint8_t *)text, length, &proposal, &status));
    CHECK(proposal.policy.cpu_trigger_bp == 500 &&
          proposal.policy.memory_trigger_bp == 25 &&
          proposal.policy.io_trigger_bp == 100 &&
          proposal.policy.confirmation_samples == 2);
    length = make_proposal(text, sizeof(text), 1, UINT32_MAX - 1U,
                           UINT32_MAX, 8000, 3000, 5000, 10);
    CHECK(l1_proposal_parse((const uint8_t *)text, length, &proposal, &status));
    CHECK(proposal.policy.generation == UINT32_MAX);

    length = make_proposal(text, sizeof(text), 2, 0, 1, 3000, 100, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_UNSUPPORTED_SCHEMA);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 499, 100, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 8001, 100, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 24, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 3001, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 100, 99, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 100, 5001, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 100, 500, 1);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 0, 1, 3000, 100, 500, 11);
    expect_parse_failure(text, length, L1_PROPOSAL_SAFETY_BOUND);
    length = make_proposal(text, sizeof(text), 1, 1, 1, 3000, 100, 500, 3);
    expect_parse_failure(text, length, L1_PROPOSAL_STALE_GENERATION);
}

static void test_adversarial_parser(void) {
    static const char *invalid[] = {
        "",
        "HMI_L1_POLICY_PROPOSAL_V2\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\nbase_generation=0\n"
        "policy_schema_version=1\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ngeneration=2\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nunknown=1\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\nX",
        "HMI_L1_POLICY_PROPOSAL_V1\r\npolicy_schema_version=1\r\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=01\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=-1\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=+1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=4294967296\n"
        "cpu_trigger_bp=3000\nmemory_trigger_bp=100\n"
        "io_trigger_bp=500\nconfirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000 \n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nEND\n",
        "HMI_L1_POLICY_PROPOSAL_V1\n\npolicy_schema_version=1\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect_parse_failure(invalid[i], strlen(invalid[i]),
                             i == 0 ? L1_PROPOSAL_RESOURCE_BOUND :
                             L1_PROPOSAL_MALFORMED);
    char embedded[sizeof(canonical)];
    memcpy(embedded, canonical, sizeof(canonical));
    embedded[10] = '\0';
    expect_parse_failure(embedded, sizeof(embedded) - 1U,
                         L1_PROPOSAL_MALFORMED);
    uint8_t oversized[L1_PROPOSAL_MAX_BYTES + 1U];
    memset(oversized, 'X', sizeof(oversized));
    expect_parse_failure(oversized, sizeof(oversized),
                         L1_PROPOSAL_RESOURCE_BOUND);
    expect_parse_failure(NULL, 1, L1_PROPOSAL_INVALID_ARGUMENT);
}

typedef struct {
    const uint8_t *bytes;
    size_t length;
    bool allow;
} VerifierContext;

static bool exact_verifier(const uint8_t *bytes, size_t length, void *context) {
    const VerifierContext *expected = context;
    return expected != NULL && expected->allow &&
        length == expected->length &&
        memcmp(bytes, expected->bytes, length) == 0;
}

static L1Observation observation(uint16_t cpu, uint16_t memory, uint16_t io) {
    return (L1Observation){cpu, memory, io, true};
}

static void expect_clone_rejection(const L1Core *core, const uint8_t *bytes,
                                   size_t length,
                                   L1ProposalTestVerifier verifier,
                                   void *context,
                                   L1ProposalStatus expected) {
    L1Core result;
    memset(&result, 0x5a, sizeof(result));
    L1Core result_before = result;
    L1Core current_before = *core;
    L1ProposalStatus status;
    CHECK(!l1_proposal_test_apply_clone(core, bytes, length, verifier, context,
                                        &result, &status));
    CHECK(status == expected);
    CHECK(memcmp(core, &current_before, sizeof(*core)) == 0);
    CHECK(memcmp(&result, &result_before, sizeof(result)) == 0);
}

static void test_authorization_and_clone(void) {
    L1Core current;
    l1_init(&current);
    for (unsigned i = 0; i < 3; ++i)
        (void)l1_step_at(&current, observation(5000, 0, 0), i * 1000U);
    CHECK(current.action == L1_PREFER_INTERACTIVE);
    L1Core current_before = current;
    VerifierContext verifier = {
        (const uint8_t *)canonical, sizeof(canonical) - 1U, true
    };
    expect_clone_rejection(&current, (const uint8_t *)canonical,
                           sizeof(canonical) - 1U, NULL, NULL,
                           L1_PROPOSAL_AUTH_UNAVAILABLE);
    verifier.allow = false;
    expect_clone_rejection(&current, (const uint8_t *)canonical,
                           sizeof(canonical) - 1U, exact_verifier, &verifier,
                           L1_PROPOSAL_AUTH_DENIED);
    verifier.allow = true;

    char rejected[512];
    size_t rejected_length = make_proposal(
        rejected, sizeof(rejected), 1, 0, 1, 499, 100, 500, 3);
    expect_clone_rejection(&current, (const uint8_t *)rejected,
                           rejected_length, exact_verifier, &verifier,
                           L1_PROPOSAL_SAFETY_BOUND);
    rejected_length = make_proposal(
        rejected, sizeof(rejected), 1, 1, 1, 3000, 100, 500, 3);
    expect_clone_rejection(&current, (const uint8_t *)rejected,
                           rejected_length, exact_verifier, &verifier,
                           L1_PROPOSAL_STALE_GENERATION);
    L1Core wrong_base = current;
    wrong_base.policy.generation = 1;
    expect_clone_rejection(&wrong_base, (const uint8_t *)canonical,
                           sizeof(canonical) - 1U, exact_verifier, &verifier,
                           L1_PROPOSAL_BASE_MISMATCH);

    char tampered[sizeof(canonical)];
    memcpy(tampered, canonical, sizeof(canonical));
    char *cpu = strstr(tampered, "cpu_trigger_bp=3000");
    CHECK(cpu != NULL);
    cpu[strlen("cpu_trigger_bp=300")] = '1';
    expect_clone_rejection(&current, (const uint8_t *)tampered,
                           sizeof(tampered) - 1U, exact_verifier, &verifier,
                           L1_PROPOSAL_AUTH_DENIED);

    const char claimed[] =
        "HMI_L1_POLICY_PROPOSAL_V1\npolicy_schema_version=1\n"
        "base_generation=0\ngeneration=1\ncpu_trigger_bp=3000\n"
        "memory_trigger_bp=100\nio_trigger_bp=500\n"
        "confirmation_samples=3\nauthorized=true\nEND\n";
    expect_clone_rejection(&current, (const uint8_t *)claimed,
                           sizeof(claimed) - 1U, exact_verifier, &verifier,
                           L1_PROPOSAL_MALFORMED);

    L1Core accepted;
    L1ProposalStatus status;
    CHECK(l1_proposal_test_apply_clone(
        &current, (const uint8_t *)canonical, sizeof(canonical) - 1U,
        exact_verifier, &verifier, &accepted, &status));
    CHECK(status == L1_PROPOSAL_OK);
    CHECK(memcmp(&current, &current_before, sizeof(current)) == 0);
    CHECK(accepted.policy.generation == 1);
    CHECK(accepted.action == L1_HOLD && accepted.candidate == L1_HOLD);
    CHECK(accepted.candidate_streak == 0 && accepted.decisions == 0);
    CHECK(accepted.has_previous_policy);
    CHECK(accepted.previous_policy.generation == 0);

    CHECK(l1_step_at(&accepted, observation(9000, 500, 9000), 0) == L1_HOLD);
    CHECK(l1_step_at(&accepted, observation(9000, 500, 9000), 1000) ==
          L1_HOLD);
    CHECK(l1_step_at(&accepted, observation(9000, 500, 9000), 2000) ==
          L1_PROTECT_MEMORY);
    CHECK(l1_rollback_policy(&accepted, 2));
    CHECK(accepted.policy.generation == 2);
    CHECK(accepted.policy.cpu_trigger_bp == 3000);
    CHECK(accepted.action == L1_HOLD && accepted.decisions == 0);

    L1Proposal parsed;
    CHECK(l1_proposal_parse((const uint8_t *)canonical,
                            sizeof(canonical) - 1U, &parsed, &status));
    CHECK(!l1_proposal_check(&accepted, &parsed, &status));
    CHECK(status == L1_PROPOSAL_BASE_MISMATCH);

    char final_generation[512];
    size_t final_length = make_proposal(
        final_generation, sizeof(final_generation), 1, UINT32_MAX - 1U,
        UINT32_MAX, 8000, 3000, 5000, 10);
    L1Core near_limit;
    l1_init(&near_limit);
    near_limit.policy.generation = UINT32_MAX - 1U;
    VerifierContext final_verifier = {
        (const uint8_t *)final_generation, final_length, true
    };
    L1Core final;
    CHECK(l1_proposal_test_apply_clone(
        &near_limit, (const uint8_t *)final_generation, final_length,
        exact_verifier, &final_verifier, &final, &status));
    CHECK(final.policy.generation == UINT32_MAX);
    CHECK(!l1_rollback_policy(&final, UINT32_MAX));
}

static void write_file(const char *path, const void *bytes, size_t length) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(descriptor >= 0);
    const uint8_t *next = bytes;
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t count = write(descriptor, next, remaining);
        CHECK(count > 0);
        next += (size_t)count;
        remaining -= (size_t)count;
    }
    CHECK(close(descriptor) == 0);
}

typedef struct {
    const char *path;
    const char *replacement;
} ReplaceContext;

static void replace_after_read(void *context) {
    ReplaceContext *replace = context;
    CHECK(rename(replace->replacement, replace->path) == 0);
}

static void test_filesystem_boundary(void) {
    char directory[] = "/tmp/hmi-l1-proposal-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], link_path[256], replacement[256], oversized[256];
    CHECK((size_t)snprintf(path, sizeof(path), "%s/proposal", directory) <
          sizeof(path));
    CHECK((size_t)snprintf(link_path, sizeof(link_path), "%s/link", directory) <
          sizeof(link_path));
    CHECK((size_t)snprintf(replacement, sizeof(replacement), "%s/new", directory) <
          sizeof(replacement));
    CHECK((size_t)snprintf(oversized, sizeof(oversized), "%s/large", directory) <
          sizeof(oversized));
    write_file(path, canonical, sizeof(canonical) - 1U);
    uint8_t bytes[L1_PROPOSAL_MAX_BYTES];
    size_t length = 0;
    L1ProposalStatus status;
    CHECK(l1_proposal_read_file(path, bytes, &length, &status));
    CHECK(length == sizeof(canonical) - 1U);
    CHECK(memcmp(bytes, canonical, length) == 0);

    CHECK(symlink(path, link_path) == 0);
    CHECK(!l1_proposal_read_file(link_path, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_TYPE);
    CHECK(!l1_proposal_read_file(directory, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_TYPE);
    CHECK(!l1_proposal_read_file("/dev/null", bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_TYPE);
    CHECK(!l1_proposal_read_file("relative", bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_PATH);
    CHECK(!l1_proposal_read_file("/tmp/../tmp/proposal", bytes, &length,
                                 &status));
    CHECK(status == L1_PROPOSAL_FILE_PATH);

    CHECK(chmod(path, 0666) == 0);
    CHECK(!l1_proposal_read_file(path, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_PERMISSION);
    CHECK(chmod(path, 0600) == 0);

    uint8_t large[L1_PROPOSAL_MAX_BYTES + 1U];
    memset(large, 'X', sizeof(large));
    write_file(oversized, large, sizeof(large));
    CHECK(!l1_proposal_read_file(oversized, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_RESOURCE_BOUND);

    char alternate[512];
    size_t alternate_length = make_proposal(
        alternate, sizeof(alternate), 1, 0, 2, 3000, 100, 500, 3);
    write_file(replacement, alternate, alternate_length);
    ReplaceContext replace = {path, replacement};
    l1_proposal_set_test_post_read_hook(replace_after_read, &replace);
    CHECK(!l1_proposal_read_file(path, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_CHANGED);
    l1_proposal_set_test_post_read_hook(NULL, NULL);

    char long_path[4100];
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1U] = '\0';
    CHECK(!l1_proposal_read_file(long_path, bytes, &length, &status));
    CHECK(status == L1_PROPOSAL_FILE_PATH);

    CHECK(unlink(path) == 0);
    CHECK(unlink(link_path) == 0);
    CHECK(unlink(oversized) == 0);
    CHECK(rmdir(directory) == 0);
}

int main(void) {
    CHECK(sizeof(L1Core) == 104);
    test_canonical_and_bounds();
    test_adversarial_parser();
    test_authorization_and_clone();
    test_filesystem_boundary();
    puts("PASS: canonical proposal, deny-by-default authorization, clone isolation, rollback, and adversarial files");
    return EXIT_SUCCESS;
}
