#define _POSIX_C_SOURCE 200809L
#include "l1.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    V1_STATE_SIZE = 64,
    V1_CRC_OFFSET = 60,
    V2_STATE_SIZE = 72,
    V2_CRC_OFFSET = 68,
    V2_FORMAT_VERSION = 2
};
static const unsigned char v1_magic[8] = {'H','M','I','L','1','S','1','\0'};
static const unsigned char v2_magic[8] = {'H','M','I','L','1','S','2','\0'};

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
        core->cpu_debounce_extra > L1_MAX_EXTRA_CONFIRMATIONS ||
        core->io_debounce_extra > L1_MAX_EXTRA_CONFIRMATIONS ||
        (core->has_previous_policy &&
         (!policy_is_storable(&core->previous_policy) ||
          core->previous_policy.generation >= core->policy.generation))) {
        set_error(error, error_size, "state violates compiled safety envelope");
        return false;
    }
    unsigned char data[V2_STATE_SIZE] = {0};
    memcpy(data, v2_magic, sizeof(v2_magic));
    put_u32(data + 8, V2_FORMAT_VERSION);
    put_u32(data + 12, V2_STATE_SIZE);
    encode_policy(data, 16, &core->policy);
    data[31] = core->cpu_debounce_extra;
    data[32] = core->io_debounce_extra;
    data[33] = core->has_previous_policy ? 1U : 0U;
    if (core->has_previous_policy) encode_policy(data, 40, &core->previous_policy);
    put_u32(data + V2_CRC_OFFSET, crc32_bytes(data, V2_CRC_OFFSET));

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
    unsigned char data[V2_STATE_SIZE + 1U];
    size_t count = fread(data, 1, sizeof(data), file);
    bool read_ok = !ferror(file);
    /* Always close the file, including when fread() reported an error. */
    if (fclose(file) != 0) read_ok = false;
    bool is_v1 = read_ok && count == V1_STATE_SIZE &&
        memcmp(data, v1_magic, sizeof(v1_magic)) == 0 &&
        get_u32(data + 8) == 1 && get_u32(data + 12) == V1_STATE_SIZE &&
        get_u32(data + V1_CRC_OFFSET) == crc32_bytes(data, V1_CRC_OFFSET);
    bool is_v2 = read_ok && count == V2_STATE_SIZE &&
        memcmp(data, v2_magic, sizeof(v2_magic)) == 0 &&
        get_u32(data + 8) == V2_FORMAT_VERSION &&
        get_u32(data + 12) == V2_STATE_SIZE &&
        get_u32(data + V2_CRC_OFFSET) == crc32_bytes(data, V2_CRC_OFFSET);
    if (!is_v1 && !is_v2) {
        set_error(error, error_size, "invalid version, size, or integrity check");
        return false;
    }
    L1Policy active = decode_policy(data, 16);
    size_t previous_offset = is_v1 ? 36U : 40U;
    size_t reserved_start = is_v1 ? 33U : 34U;
    size_t reserved_middle_end = is_v1 ? 36U : 40U;
    size_t reserved_tail_start = is_v1 ? 51U : 55U;
    size_t crc_offset = is_v1 ? V1_CRC_OFFSET : V2_CRC_OFFSET;
    unsigned has_previous_byte = data[is_v1 ? 32U : 33U];
    L1Policy previous = decode_policy(data, previous_offset);
    bool has_previous = has_previous_byte == 1U;
    for (size_t i = reserved_start; i < reserved_middle_end; ++i) {
        if (data[i] != 0) {
            set_error(error, error_size, "nonzero reserved state field");
            return false;
        }
    }
    for (size_t i = reserved_tail_start; i < crc_offset; ++i) {
        if (data[i] != 0) {
            set_error(error, error_size, "nonzero reserved state field");
            return false;
        }
    }
    if (has_previous_byte > 1U || !policy_is_storable(&active) ||
        (is_v1 && data[31] > L1_MAX_EXTRA_CONFIRMATIONS) ||
        (is_v2 && (data[31] > L1_MAX_EXTRA_CONFIRMATIONS ||
                   data[32] > L1_MAX_EXTRA_CONFIRMATIONS)) ||
        (has_previous &&
         (!policy_is_storable(&previous) ||
          previous.generation >= active.generation))) {
        set_error(error, error_size, "state violates compiled safety envelope");
        return false;
    }
    if (!has_previous) {
        for (size_t i = previous_offset; i < previous_offset + 15U; ++i) {
            if (data[i] != 0) {
                set_error(error, error_size, "unexpected previous policy data");
                return false;
            }
        }
    }
    core->policy = active;
    core->previous_policy = previous;
    core->has_previous_policy = has_previous;
    if (is_v2) {
        core->cpu_debounce_extra = data[31];
        core->io_debounce_extra = data[32];
    }
    return true;
}
