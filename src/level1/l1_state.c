#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { STATE_SIZE = 64, STATE_CRC_OFFSET = 60, STATE_FORMAT_VERSION = 1 };
static const unsigned char state_magic[8] = {'H','M','I','L','1','S','1','\0'};

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0) snprintf(error, size, "%s", message);
}

static void put_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8U * i));
}

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_bytes(const unsigned char *data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static bool policy_is_storable(const L1Policy *policy) {
    if (l1_policy_valid(policy)) return true;
    L1Policy defaults = l1_default_policy();
    return policy != NULL && policy->generation == 0 &&
           policy->schema_version == defaults.schema_version &&
           policy->cpu_trigger_bp == defaults.cpu_trigger_bp &&
           policy->memory_trigger_bp == defaults.memory_trigger_bp &&
           policy->io_trigger_bp == defaults.io_trigger_bp &&
           policy->confirmation_samples == defaults.confirmation_samples;
}

static void encode_policy(unsigned char *data, size_t offset,
                          const L1Policy *policy) {
    put_u32(data + offset, policy->schema_version);
    put_u32(data + offset + 4, policy->generation);
    put_u16(data + offset + 8, policy->cpu_trigger_bp);
    put_u16(data + offset + 10, policy->memory_trigger_bp);
    put_u16(data + offset + 12, policy->io_trigger_bp);
    data[offset + 14] = policy->confirmation_samples;
}

static L1Policy decode_policy(const unsigned char *data, size_t offset) {
    return (L1Policy) {
        .schema_version = get_u32(data + offset),
        .generation = get_u32(data + offset + 4),
        .cpu_trigger_bp = get_u16(data + offset + 8),
        .memory_trigger_bp = get_u16(data + offset + 10),
        .io_trigger_bp = get_u16(data + offset + 12),
        .confirmation_samples = data[offset + 14]
    };
}

static bool write_all(int descriptor, const unsigned char *data, size_t size) {
    while (size != 0) {
        ssize_t written = write(descriptor, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool sync_parent_directory(const char *path) {
    const char *slash = strrchr(path, '/');
    char *directory = NULL;
    const char *selected = ".";
    if (slash != NULL) {
        size_t length = slash == path ? 1U : (size_t)(slash - path);
        directory = malloc(length + 1U);
        if (directory == NULL) return false;
        memcpy(directory, path, length);
        directory[length] = '\0';
        selected = directory;
    }
    int descriptor = open(selected, O_RDONLY);
    free(directory);
    if (descriptor < 0) return false;
    bool ok = fsync(descriptor) == 0;
    if (close(descriptor) != 0) ok = false;
    return ok;
}

bool l1_state_save_atomic(const char *path, const L1Core *core,
                          char *error, size_t error_size) {
    if (path == NULL || *path == '\0' || core == NULL ||
        !policy_is_storable(&core->policy) ||
        core->debounce_extra > L1_MAX_EXTRA_CONFIRMATIONS ||
        (core->has_previous_policy &&
         (!policy_is_storable(&core->previous_policy) ||
          core->previous_policy.generation >= core->policy.generation))) {
        set_error(error, error_size, "state violates compiled safety envelope");
        return false;
    }
    unsigned char data[STATE_SIZE] = {0};
    memcpy(data, state_magic, sizeof(state_magic));
    put_u32(data + 8, STATE_FORMAT_VERSION);
    put_u32(data + 12, STATE_SIZE);
    encode_policy(data, 16, &core->policy);
    data[31] = core->debounce_extra;
    data[32] = core->has_previous_policy ? 1U : 0U;
    if (core->has_previous_policy) encode_policy(data, 36, &core->previous_policy);
    put_u32(data + STATE_CRC_OFFSET, crc32_bytes(data, STATE_CRC_OFFSET));

    size_t path_length = strlen(path);
    if (path_length > SIZE_MAX - 12U) {
        set_error(error, error_size, "state path is too long");
        return false;
    }
    char *temporary = malloc(path_length + 12U);
    if (temporary == NULL) {
        set_error(error, error_size, "cannot allocate temporary path");
        return false;
    }
    snprintf(temporary, path_length + 12U, "%s.tmp.XXXXXX", path);
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        free(temporary);
        set_error(error, error_size, "cannot create temporary state file");
        return false;
    }
    bool ok = write_all(descriptor, data, sizeof(data)) && fsync(descriptor) == 0;
    if (close(descriptor) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (ok && !sync_parent_directory(path)) ok = false;
    if (!ok) {
        (void)unlink(temporary);
        set_error(error, error_size, "atomic state persistence failed");
    }
    free(temporary);
    return ok;
}

bool l1_state_load(const char *path, L1Core *core,
                   char *error, size_t error_size) {
    if (core == NULL) return false;
    l1_init(core); /* Every failure recovers to compiled defaults and HOLD. */
    if (path == NULL || *path == '\0') {
        set_error(error, error_size, "invalid state path");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "cannot open state file");
        return false;
    }
    unsigned char data[STATE_SIZE + 1U];
    size_t count = fread(data, 1, sizeof(data), file);
    bool read_ok = !ferror(file) && fclose(file) == 0;
    if (!read_ok || count != STATE_SIZE ||
        memcmp(data, state_magic, sizeof(state_magic)) != 0 ||
        get_u32(data + 8) != STATE_FORMAT_VERSION ||
        get_u32(data + 12) != STATE_SIZE ||
        get_u32(data + STATE_CRC_OFFSET) !=
            crc32_bytes(data, STATE_CRC_OFFSET)) {
        set_error(error, error_size, "invalid version, size, or integrity check");
        return false;
    }
    for (size_t i = 33; i < 36; ++i) {
        if (data[i] != 0) {
            set_error(error, error_size, "nonzero reserved state field");
            return false;
        }
    }
    for (size_t i = 51; i < STATE_CRC_OFFSET; ++i) {
        if (data[i] != 0) {
            set_error(error, error_size, "nonzero reserved state field");
            return false;
        }
    }
    L1Policy active = decode_policy(data, 16);
    L1Policy previous = decode_policy(data, 36);
    bool has_previous = data[32] == 1U;
    if (data[32] > 1U || !policy_is_storable(&active) ||
        data[31] > L1_MAX_EXTRA_CONFIRMATIONS ||
        (has_previous &&
         (!policy_is_storable(&previous) ||
          previous.generation >= active.generation))) {
        set_error(error, error_size, "state violates compiled safety envelope");
        return false;
    }
    if (!has_previous) {
        for (size_t i = 36; i < 51; ++i) {
            if (data[i] != 0) {
                set_error(error, error_size, "unexpected previous policy data");
                return false;
            }
        }
    }
    core->policy = active;
    core->previous_policy = previous;
    core->has_previous_policy = has_previous;
    core->debounce_extra = data[31];
    return true;
}
