#define _POSIX_C_SOURCE 200809L
#include "l1_proposal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *key;
    uint32_t maximum;
} NumericField;

static void set_status(L1ProposalStatus *status, L1ProposalStatus value) {
    if (status != NULL) *status = value;
}

const char *l1_proposal_status_name(L1ProposalStatus status) {
    switch (status) {
        case L1_PROPOSAL_OK: return "ok";
        case L1_PROPOSAL_INVALID_ARGUMENT: return "invalid-argument";
        case L1_PROPOSAL_RESOURCE_BOUND: return "resource-bound";
        case L1_PROPOSAL_MALFORMED: return "malformed-envelope";
        case L1_PROPOSAL_UNSUPPORTED_SCHEMA: return "unsupported-schema";
        case L1_PROPOSAL_SAFETY_BOUND: return "safety-bound";
        case L1_PROPOSAL_BASE_MISMATCH: return "base-generation-mismatch";
        case L1_PROPOSAL_STALE_GENERATION: return "stale-generation";
        case L1_PROPOSAL_AUTH_UNAVAILABLE: return "authorization-unavailable";
        case L1_PROPOSAL_AUTH_DENIED: return "authorization-denied";
        case L1_PROPOSAL_FILE_PATH: return "unsafe-path";
        case L1_PROPOSAL_FILE_TYPE: return "unsupported-file-type";
        case L1_PROPOSAL_FILE_PERMISSION: return "unsafe-file-permission";
        case L1_PROPOSAL_FILE_CHANGED: return "file-changed";
        case L1_PROPOSAL_IO: return "file-io";
        default: return "unknown";
    }
}

static bool decimal_u32(const uint8_t *text, size_t length, uint32_t maximum,
                        uint32_t *value) {
    if (length == 0 || value == NULL ||
        (length > 1 && text[0] == (uint8_t)'0')) return false;
    uint32_t result = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < (uint8_t)'0' || text[i] > (uint8_t)'9') return false;
        uint32_t digit = (uint32_t)(text[i] - (uint8_t)'0');
        if (result > (maximum - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

static bool exact_line(const uint8_t *bytes, size_t start, size_t end,
                       const char *expected) {
    size_t length = end - start;
    return strlen(expected) == length &&
        memcmp(bytes + start, expected, length) == 0;
}

bool l1_proposal_parse(const uint8_t *bytes, size_t length,
                       L1Proposal *proposal, L1ProposalStatus *status) {
    if (bytes == NULL || proposal == NULL) {
        set_status(status, L1_PROPOSAL_INVALID_ARGUMENT);
        return false;
    }
    if (length == 0 || length > L1_PROPOSAL_MAX_BYTES) {
        set_status(status, L1_PROPOSAL_RESOURCE_BOUND);
        return false;
    }
    size_t starts[L1_PROPOSAL_MAX_LINES] = {0};
    size_t ends[L1_PROPOSAL_MAX_LINES] = {0};
    size_t start = 0, lines = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (byte == 0 || byte == (uint8_t)'\r' || byte == (uint8_t)' ' ||
            byte == (uint8_t)'\t' || byte < 0x0aU || byte > 0x7eU) {
            set_status(status, L1_PROPOSAL_MALFORMED);
            return false;
        }
        if (byte == (uint8_t)'\n') {
            if (lines >= L1_PROPOSAL_MAX_LINES || i == start) {
                set_status(status, L1_PROPOSAL_MALFORMED);
                return false;
            }
            starts[lines] = start;
            ends[lines] = i;
            lines++;
            start = i + 1U;
        }
    }
    if (start != length || lines != 9U ||
        !exact_line(bytes, starts[0], ends[0],
                    "HMI_L1_POLICY_PROPOSAL_V1") ||
        !exact_line(bytes, starts[8], ends[8], "END")) {
        set_status(status, L1_PROPOSAL_MALFORMED);
        return false;
    }

    static const NumericField fields[7] = {
        {"policy_schema_version=", UINT32_MAX},
        {"base_generation=", UINT32_MAX},
        {"generation=", UINT32_MAX},
        {"cpu_trigger_bp=", UINT16_MAX},
        {"memory_trigger_bp=", UINT16_MAX},
        {"io_trigger_bp=", UINT16_MAX},
        {"confirmation_samples=", UINT8_MAX}
    };
    uint32_t values[7] = {0};
    for (size_t field = 0; field < 7U; ++field) {
        size_t line = field + 1U;
        size_t key_length = strlen(fields[field].key);
        size_t line_length = ends[line] - starts[line];
        if (line_length <= key_length ||
            memcmp(bytes + starts[line], fields[field].key, key_length) != 0 ||
            !decimal_u32(bytes + starts[line] + key_length,
                         line_length - key_length, fields[field].maximum,
                         &values[field])) {
            set_status(status, L1_PROPOSAL_MALFORMED);
            return false;
        }
    }
    if (values[0] != 1U) {
        set_status(status, L1_PROPOSAL_UNSUPPORTED_SCHEMA);
        return false;
    }
    L1Proposal parsed = {
        .base_generation = values[1],
        .policy = {
            .schema_version = values[0],
            .generation = values[2],
            .cpu_trigger_bp = (uint16_t)values[3],
            .memory_trigger_bp = (uint16_t)values[4],
            .io_trigger_bp = (uint16_t)values[5],
            .confirmation_samples = (uint8_t)values[6]
        }
    };
    if (!l1_policy_valid(&parsed.policy)) {
        set_status(status, L1_PROPOSAL_SAFETY_BOUND);
        return false;
    }
    if (parsed.policy.generation <= parsed.base_generation) {
        set_status(status, L1_PROPOSAL_STALE_GENERATION);
        return false;
    }
    *proposal = parsed;
    set_status(status, L1_PROPOSAL_OK);
    return true;
}

bool l1_proposal_check(const L1Core *current, const L1Proposal *proposal,
                       L1ProposalStatus *status) {
    if (current == NULL || proposal == NULL) {
        set_status(status, L1_PROPOSAL_INVALID_ARGUMENT);
        return false;
    }
    if (proposal->base_generation != current->policy.generation) {
        set_status(status, L1_PROPOSAL_BASE_MISMATCH);
        return false;
    }
    if (proposal->policy.generation <= current->policy.generation) {
        set_status(status, L1_PROPOSAL_STALE_GENERATION);
        return false;
    }
    if (!l1_policy_valid(&proposal->policy)) {
        set_status(status, L1_PROPOSAL_SAFETY_BOUND);
        return false;
    }
    set_status(status, L1_PROPOSAL_OK);
    return true;
}

static bool safe_absolute_path(const char *path) {
    if (path == NULL || path[0] != '/') return false;
    size_t length = strnlen(path, 4096U);
    if (length < 2U || length >= 4096U || path[length - 1U] == '/')
        return false;
    const char *component = path + 1;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t component_length = end == NULL ? strlen(component) :
            (size_t)(end - component);
        if (component_length == 0U ||
            (component_length == 1U && component[0] == '.') ||
            (component_length == 2U && component[0] == '.' &&
             component[1] == '.')) return false;
        if (end == NULL) break;
        component = end + 1;
    }
    return true;
}

static int open_no_symlink_path(const char *path, L1ProposalStatus *status) {
    if (!safe_absolute_path(path)) {
        set_status(status, L1_PROPOSAL_FILE_PATH);
        return -1;
    }
    char local[4096];
    size_t length = strlen(path);
    memcpy(local, path, length + 1U);
    int parent = open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (parent < 0) {
        set_status(status, L1_PROPOSAL_IO);
        return -1;
    }
    char *component = local + 1;
    for (;;) {
        char *slash = strchr(component, '/');
        bool final = slash == NULL;
        if (!final) *slash = '\0';
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (!final) flags |= O_DIRECTORY;
        int next = openat(parent, component, flags);
        int open_error = errno;
        if (close(parent) != 0 && next >= 0) {
            (void)close(next);
            set_status(status, L1_PROPOSAL_IO);
            return -1;
        }
        if (next < 0) {
            set_status(status,
                       open_error == ELOOP || open_error == ENOTDIR ?
                       L1_PROPOSAL_FILE_TYPE : L1_PROPOSAL_IO);
            return -1;
        }
        if (final) return next;
        parent = next;
        component = slash + 1;
    }
}

static bool same_file(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_size == right->st_size &&
        left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
        left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
        left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
        left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

#ifdef L1_TESTING
static L1ProposalTestHook post_read_hook;
static void *post_read_context;

void l1_proposal_set_test_post_read_hook(L1ProposalTestHook hook,
                                         void *context) {
    post_read_hook = hook;
    post_read_context = context;
}
#endif

bool l1_proposal_read_file(const char *path,
                           uint8_t bytes[L1_PROPOSAL_MAX_BYTES],
                           size_t *length, L1ProposalStatus *status) {
    if (path == NULL || bytes == NULL || length == NULL) {
        set_status(status, L1_PROPOSAL_INVALID_ARGUMENT);
        return false;
    }
    struct stat opened_before, opened_after, path_after;
    int descriptor = open_no_symlink_path(path, status);
    if (descriptor < 0) return false;
    if (fstat(descriptor, &opened_before) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        set_status(status, L1_PROPOSAL_IO);
        return false;
    }
    if (!S_ISREG(opened_before.st_mode)) {
        (void)close(descriptor);
        set_status(status, L1_PROPOSAL_FILE_TYPE);
        return false;
    }
    if (opened_before.st_uid != geteuid() ||
        (opened_before.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        (void)close(descriptor);
        set_status(status, L1_PROPOSAL_FILE_PERMISSION);
        return false;
    }
    if (opened_before.st_size <= 0 ||
        opened_before.st_size > L1_PROPOSAL_MAX_BYTES) {
        (void)close(descriptor);
        set_status(status, L1_PROPOSAL_RESOURCE_BOUND);
        return false;
    }
    size_t total = 0;
    while (total < (size_t)opened_before.st_size) {
        ssize_t count = read(descriptor, bytes + total,
                             (size_t)opened_before.st_size - total);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)close(descriptor);
            set_status(status, L1_PROPOSAL_IO);
            return false;
        }
        total += (size_t)count;
    }
    uint8_t extra;
    ssize_t trailing;
    do {
        trailing = read(descriptor, &extra, 1);
    } while (trailing < 0 && errno == EINTR);
    bool metadata_ok = trailing == 0 &&
        fstat(descriptor, &opened_after) == 0;
    if (close(descriptor) != 0) metadata_ok = false;
#ifdef L1_TESTING
    if (post_read_hook != NULL) post_read_hook(post_read_context);
#endif
    int path_descriptor = metadata_ok ? open_no_symlink_path(path, status) : -1;
    bool path_ok = path_descriptor >= 0 &&
        fstat(path_descriptor, &path_after) == 0;
    if (path_descriptor >= 0 && close(path_descriptor) != 0) path_ok = false;
    if (!metadata_ok || !path_ok ||
        !same_file(&opened_before, &opened_after) ||
        !same_file(&opened_after, &path_after)) {
        set_status(status, L1_PROPOSAL_FILE_CHANGED);
        return false;
    }
    *length = total;
    set_status(status, L1_PROPOSAL_OK);
    return true;
}

#ifdef L1_TESTING
bool l1_proposal_test_apply_clone(
    const L1Core *current, const uint8_t *bytes, size_t length,
    L1ProposalTestVerifier verifier, void *verifier_context,
    L1Core *result, L1ProposalStatus *status) {
    if (current == NULL || bytes == NULL || result == NULL ||
        current == result) {
        set_status(status, L1_PROPOSAL_INVALID_ARGUMENT);
        return false;
    }
    if (length == 0 || length > L1_PROPOSAL_MAX_BYTES) {
        set_status(status, L1_PROPOSAL_RESOURCE_BOUND);
        return false;
    }
    uint8_t exact_bytes[L1_PROPOSAL_MAX_BYTES];
    memcpy(exact_bytes, bytes, length);
    L1Proposal proposal;
    if (!l1_proposal_parse(exact_bytes, length, &proposal, status) ||
        !l1_proposal_check(current, &proposal, status)) return false;
    if (verifier == NULL) {
        set_status(status, L1_PROPOSAL_AUTH_UNAVAILABLE);
        return false;
    }
    if (!verifier(exact_bytes, length, verifier_context)) {
        set_status(status, L1_PROPOSAL_AUTH_DENIED);
        return false;
    }
    L1Core candidate = *current;
    if (!l1_apply_policy(&candidate, &proposal.policy)) {
        set_status(status, L1_PROPOSAL_SAFETY_BOUND);
        return false;
    }
    *result = candidate;
    set_status(status, L1_PROPOSAL_OK);
    return true;
}
#endif
