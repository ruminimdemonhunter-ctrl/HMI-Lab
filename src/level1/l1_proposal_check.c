#include "l1_proposal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool decimal_u32(const char *text, uint32_t *value) {
    if (text == NULL || *text == '\0' || value == NULL ||
        (text[0] == '0' && text[1] != '\0')) return false;
    uint32_t result = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        uint32_t digit = (uint32_t)(*p - '0');
        if (result > (UINT32_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

static int reject(L1ProposalStatus status) {
    printf("PROPOSAL_VALIDATION:REJECT reason=%s authority=none"
           " production_apply=blocked\n",
           l1_proposal_status_name(status));
    puts("PRODUCTION_AUTHORIZATION:BLOCKED\nL2_DEPLOYMENT:NOT_AUTHORIZED");
    return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    uint32_t current_generation;
    if (argc != 4 || strcmp(argv[2], "--current-generation") != 0 ||
        !decimal_u32(argv[3], &current_generation)) {
        fputs("usage: ./l1_proposal_check ABSOLUTE_PATH"
              " --current-generation UINT32\n", stderr);
        return EXIT_FAILURE;
    }
    uint8_t bytes[L1_PROPOSAL_MAX_BYTES];
    size_t length = 0;
    L1ProposalStatus status = L1_PROPOSAL_INVALID_ARGUMENT;
    if (!l1_proposal_read_file(argv[1], bytes, &length, &status))
        return reject(status);
    L1Proposal proposal;
    if (!l1_proposal_parse(bytes, length, &proposal, &status))
        return reject(status);
    L1Core current;
    l1_init(&current);
    current.policy.generation = current_generation;
    if (!l1_proposal_check(&current, &proposal, &status))
        return reject(status);
    printf("PROPOSAL envelope=canonical schema=%" PRIu32
           " base_generation=%" PRIu32 " generation=%" PRIu32
           " safety=PASS generation_check=PASS bytes=%zu\n",
           proposal.policy.schema_version, proposal.base_generation,
           proposal.policy.generation, length);
    puts("AUTHORIZATION verdict=DENY reason=authorization-unavailable"
         " verifier=none test_only=no authority=none production_apply=blocked");
    puts("PROPOSAL_VALIDATION:PASS_OFFLINE_ONLY\n"
         "PRODUCTION_AUTHORIZATION:BLOCKED\n"
         "L2_DEPLOYMENT:NOT_AUTHORIZED");
    return EXIT_SUCCESS;
}
