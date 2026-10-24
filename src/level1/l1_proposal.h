#ifndef HMI_L1_PROPOSAL_H
#define HMI_L1_PROPOSAL_H

#include "l1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    L1_PROPOSAL_MAX_BYTES = 4096,
    L1_PROPOSAL_MAX_LINES = 10
};

typedef struct {
    uint32_t base_generation;
    L1Policy policy;
} L1Proposal;

typedef enum {
    L1_PROPOSAL_OK = 0,
    L1_PROPOSAL_INVALID_ARGUMENT,
    L1_PROPOSAL_RESOURCE_BOUND,
    L1_PROPOSAL_MALFORMED,
    L1_PROPOSAL_UNSUPPORTED_SCHEMA,
    L1_PROPOSAL_SAFETY_BOUND,
    L1_PROPOSAL_BASE_MISMATCH,
    L1_PROPOSAL_STALE_GENERATION,
    L1_PROPOSAL_AUTH_UNAVAILABLE,
    L1_PROPOSAL_AUTH_DENIED,
    L1_PROPOSAL_FILE_PATH,
    L1_PROPOSAL_FILE_TYPE,
    L1_PROPOSAL_FILE_PERMISSION,
    L1_PROPOSAL_FILE_CHANGED,
    L1_PROPOSAL_IO
} L1ProposalStatus;

const char *l1_proposal_status_name(L1ProposalStatus status);
bool l1_proposal_parse(const uint8_t *bytes, size_t length,
                       L1Proposal *proposal, L1ProposalStatus *status);
bool l1_proposal_check(const L1Core *current, const L1Proposal *proposal,
                       L1ProposalStatus *status);
bool l1_proposal_read_file(const char *path,
                           uint8_t bytes[L1_PROPOSAL_MAX_BYTES],
                           size_t *length, L1ProposalStatus *status);

#ifdef L1_TESTING
typedef bool (*L1ProposalTestVerifier)(const uint8_t *bytes, size_t length,
                                      void *context);
typedef void (*L1ProposalTestHook)(void *context);

void l1_proposal_set_test_post_read_hook(L1ProposalTestHook hook,
                                         void *context);
bool l1_proposal_test_apply_clone(
    const L1Core *current, const uint8_t *bytes, size_t length,
    L1ProposalTestVerifier verifier, void *verifier_context,
    L1Core *result, L1ProposalStatus *status);
#endif

#endif
