#ifndef HMI_L2_STATE_H
#define HMI_L2_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { L2_WORKSPACES=4, L2_NODE_CAP=16, L2_EDGE_CAP=32, L2_TRIPLE_CAP=16, L2_PENDING_CAP=8 };

typedef enum { L2_QUALITY_OK=0, L2_QUALITY_ABSENT, L2_QUALITY_INVALID, L2_QUALITY_RESET } L2Quality;
typedef enum { L2_EVENT_SESSION_BEGIN=0, L2_EVENT_SESSION_END, L2_EVENT_ROLE, L2_EVENT_CONTEXT, L2_EVENT_QUERY, L2_EVENT_GAP, L2_EVENT_RESET } L2EventKind;
typedef enum { L2_ROLE_NONE=0, L2_ROLE_CODE_EDIT, L2_ROLE_BUILD_TEST, L2_ROLE_DOC_VIEW, L2_ROLE_REFERENCES, L2_ROLE_CANVAS, L2_ROLE_ASSET_GROUP, L2_ROLE_EXPORT, L2_ROLE_OTHER } L2Role;
typedef enum { L2_ACTIVITY_NONE=0, L2_ACTIVITY_IDLE, L2_ACTIVITY_INTERACTIVE, L2_ACTIVITY_BACKGROUND, L2_ACTIVITY_MULTI_TASK, L2_ACTIVITY_UNKNOWN } L2Activity;
typedef enum { L2_CONDITION_NONE=0, L2_CONDITION_QUIET, L2_CONDITION_CPU_PRESSURE, L2_CONDITION_MEMORY_PRESSURE, L2_CONDITION_IO_PRESSURE, L2_CONDITION_MIXED_PRESSURE, L2_CONDITION_UNKNOWN, L2_CONDITION_UNRELIABLE } L2Condition;
typedef enum { L2_CONTEXT_UNKNOWN=0, L2_CONTEXT_IDLE, L2_CONTEXT_FOCUSED, L2_CONTEXT_MULTI_TASK, L2_CONTEXT_BACKGROUND_BUSY, L2_CONTEXT_RESOURCE_CONSTRAINED, L2_CONTEXT_TRANSITION } L2Context;
typedef enum { L2_RESULT_NONE=0, L2_RESULT_CANDIDATE, L2_RESULT_BLOCKED } L2ResultStatus;
typedef enum { L2_REASON_NONE=0, L2_REASON_WARMING_UP, L2_REASON_STALE_CONTEXT, L2_REASON_INVALID_CONTEXT, L2_REASON_RESOURCE_CONSTRAINED } L2Reason;
typedef enum { L2_STEP_ACCEPTED=0, L2_STEP_QUERY_RESULT, L2_STEP_INVALID_QUALITY, L2_STEP_REJECTED } L2StepStatus;

typedef struct {
    uint64_t seq;
    uint64_t monotonic_ms;
    L2Quality quality;
    L2EventKind kind;
    uint8_t workspace;
    L2Role role;
    L2Activity activity;
    L2Condition condition;
    bool simulated;
} L2Event;

typedef struct { uint8_t workspace,role,used,reserved; uint16_t count,reserved2; uint32_t last_generation; } L2Node;
typedef struct { uint8_t workspace,from_role,to_role,used; uint16_t count,reserved; uint32_t last_generation; } L2Edge;
typedef struct { uint8_t workspace,r1,r2,r3,used,reserved1; uint16_t count; uint32_t last_generation; } L2Triple;
typedef struct { uint16_t completed_sessions,accepted_transitions; } L2Workspace;

typedef struct {
    L2Node nodes[L2_NODE_CAP];
    L2Edge edges[L2_EDGE_CAP];
    L2Triple triples[L2_TRIPLE_CAP];
    L2Workspace workspaces[L2_WORKSPACES];
    uint8_t pending_roles[L2_PENDING_CAP];
    uint8_t pending_count;
    uint8_t pending_workspace;
    uint8_t session_open;
    uint8_t context_valid;
    L2Context context;
    uint8_t stream_initialized;
    uint8_t reserved[2];
    uint32_t completed_session_generation;
    uint64_t expected_seq;
    uint64_t last_valid_ms;
    uint64_t context_ms;
    uint32_t invalid_events;
    uint32_t aborted_sessions;
} L2State;

typedef struct {
    L2ResultStatus status;
    L2Context context;
    L2Reason reason;
    uint8_t workspace;
    L2Role roles[3];
    uint16_t completed_sessions;
    uint16_t accepted_transitions;
    uint16_t triple_count;
    bool requires_user_approval;
} L2QueryResult;

void l2_init(L2State *state);
L2StepStatus l2_step(L2State *state,const L2Event *event,L2QueryResult *result);
L2QueryResult l2_query_workspace(const L2State *state,uint8_t workspace,uint64_t now_ms);
const char *l2_role_name(L2Role role);
const char *l2_context_name(L2Context context);
const char *l2_result_name(L2ResultStatus status);
const char *l2_reason_name(L2Reason reason);

#endif
