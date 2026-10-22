#define _POSIX_C_SOURCE 200809L
#include "l1.h"

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

static void corrupt_byte(const char *path, long offset) {
    FILE *file = fopen(path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, offset, SEEK_SET) == 0);
    int value = fgetc(file);
    CHECK(value != EOF);
    CHECK(fseek(file, offset, SEEK_SET) == 0);
    CHECK(fputc(value ^ 0x5a, file) != EOF);
    CHECK(fclose(file) == 0);
}

static void put_u32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8U * i));
}

static void put_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void encode_test_policy(unsigned char *data, size_t offset,
                               const L1Policy *policy) {
    put_u32(data + offset, policy->schema_version);
    put_u32(data + offset + 4, policy->generation);
    put_u16(data + offset + 8, policy->cpu_trigger_bp);
    put_u16(data + offset + 10, policy->memory_trigger_bp);
    put_u16(data + offset + 12, policy->io_trigger_bp);
    data[offset + 14] = policy->confirmation_samples;
}

static uint32_t test_crc32(const unsigned char *data, size_t size) {
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

static void rewrite_as_unsupported_version(const char *path) {
    unsigned char data[72];
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fread(data, 1, sizeof(data), file) == sizeof(data));
    CHECK(fclose(file) == 0);
    put_u32(data + 8, 99);
    put_u32(data + 68, test_crc32(data, 68));
    file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    CHECK(fclose(file) == 0);
}

static void write_v1_state(const char *path, const L1Policy *active,
                           const L1Policy *previous, uint8_t legacy_extra) {
    unsigned char data[64] = {0};
    static const unsigned char magic[8] =
        {'H','M','I','L','1','S','1','\0'};
    memcpy(data, magic, sizeof(magic));
    put_u32(data + 8, 1);
    put_u32(data + 12, 64);
    encode_test_policy(data, 16, active);
    data[31] = legacy_extra;
    data[32] = previous != NULL ? 1U : 0U;
    if (previous != NULL) encode_test_policy(data, 36, previous);
    put_u32(data + 60, test_crc32(data, 60));
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    CHECK(fclose(file) == 0);
}

static void assert_safe_default(const L1Core *core) {
    L1Policy defaults = l1_default_policy();
    CHECK(core->policy.generation == 0);
    CHECK(core->policy.cpu_trigger_bp == defaults.cpu_trigger_bp);
    CHECK(core->action == L1_HOLD);
    CHECK(core->debounce_extra == 0);
    CHECK(core->cpu_debounce_extra == 0);
    CHECK(core->io_debounce_extra == 0);
    CHECK(core->adaptation_mode == L1_ADAPT_RISK_AWARE);
    CHECK(!core->has_previous_policy);
    CHECK(!core->monotonic_initialized);
}

int main(void) {
    char directory[] = "/tmp/hmi-l1-state-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    CHECK((size_t)snprintf(path, sizeof(path), "%s/state.bin", directory) <
          sizeof(path));
    char error[128] = {0};

    L1Core original;
    l1_init(&original);
    L1Policy first = l1_default_policy();
    first.generation = 1;
    first.cpu_trigger_bp = 2200;
    L1Policy second = first;
    second.generation = 2;
    second.cpu_trigger_bp = 4400;
    CHECK(l1_apply_policy(&original, &first));
    CHECK(l1_apply_policy(&original, &second));
    original.cpu_debounce_extra = 2;
    original.io_debounce_extra = 1;
    CHECK(l1_state_save_atomic(path, &original, error, sizeof(error)));

    struct stat metadata;
    CHECK(stat(path, &metadata) == 0);
    CHECK(metadata.st_size == 72);
    CHECK((metadata.st_mode & (S_IRWXG | S_IRWXO)) == 0);

    L1Core recovered;
    CHECK(l1_state_load(path, &recovered, error, sizeof(error)));
    CHECK(recovered.policy.generation == 2);
    CHECK(recovered.policy.cpu_trigger_bp == 4400);
    CHECK(recovered.has_previous_policy);
    CHECK(recovered.previous_policy.generation == 1);
    CHECK(recovered.cpu_debounce_extra == 2);
    CHECK(recovered.io_debounce_extra == 1);
    CHECK(recovered.debounce_extra == 0);
    CHECK(recovered.adaptation_mode == L1_ADAPT_RISK_AWARE);
    CHECK(recovered.action == L1_HOLD && recovered.decisions == 0);
    CHECK(!recovered.monotonic_initialized);
    CHECK(l1_rollback_policy(&recovered, 3));
    CHECK(recovered.policy.cpu_trigger_bp == 2200);

    CHECK(l1_state_save_atomic(path, &recovered, error, sizeof(error)));
    L1Core replaced;
    CHECK(l1_state_load(path, &replaced, error, sizeof(error)));
    CHECK(replaced.policy.generation == 3);
    CHECK(replaced.policy.cpu_trigger_bp == 2200);

    /* V1 migration retains validated policy memory but never reinterprets the
       old global debounce value as either V0.5 action-specific value. */
    write_v1_state(path, &second, &first, 2);
    CHECK(l1_state_load(path, &replaced, error, sizeof(error)));
    CHECK(replaced.policy.generation == 2);
    CHECK(replaced.previous_policy.generation == 1);
    CHECK(replaced.cpu_debounce_extra == 0);
    CHECK(replaced.io_debounce_extra == 0);
    CHECK(replaced.debounce_extra == 0);
    CHECK(replaced.adaptation_mode == L1_ADAPT_RISK_AWARE);
    CHECK(l1_state_save_atomic(path, &replaced, error, sizeof(error)));
    CHECK(stat(path, &metadata) == 0 && metadata.st_size == 72);

    replaced.cpu_debounce_extra = L1_MAX_EXTRA_CONFIRMATIONS + 1U;
    CHECK(!l1_state_save_atomic(path, &replaced, error, sizeof(error)));
    replaced.cpu_debounce_extra = 0;

    corrupt_byte(path, 24);
    replaced.action = L1_PROTECT_MEMORY;
    CHECK(!l1_state_load(path, &replaced, error, sizeof(error)));
    assert_safe_default(&replaced);

    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("tiny", 1, 4, file) == 4);
    CHECK(fclose(file) == 0);
    CHECK(!l1_state_load(path, &replaced, error, sizeof(error)));
    assert_safe_default(&replaced);

    CHECK(l1_state_save_atomic(path, &original, error, sizeof(error)));
    rewrite_as_unsupported_version(path);
    CHECK(!l1_state_load(path, &replaced, error, sizeof(error)));
    assert_safe_default(&replaced);

    CHECK(l1_state_save_atomic(path, &original, error, sizeof(error)));
    file = fopen(path, "ab");
    CHECK(file != NULL);
    CHECK(fputc(0, file) != EOF);
    CHECK(fclose(file) == 0);
    CHECK(!l1_state_load(path, &replaced, error, sizeof(error)));
    assert_safe_default(&replaced);

    char missing[256];
    CHECK((size_t)snprintf(missing, sizeof(missing), "%s/missing/state.bin",
                           directory) < sizeof(missing));
    CHECK(!l1_state_save_atomic(missing, &original, error, sizeof(error)));
    CHECK(!l1_state_load(missing, &replaced, error, sizeof(error)));
    assert_safe_default(&replaced);

    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    puts("PASS: v2 state, v1 migration, CRC, atomic replacement, permissions, and safe recovery");
    return 0;
}
