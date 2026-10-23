#ifndef HMI_L1_LINUX_H
#define HMI_L1_LINUX_H

#include "l1.h"

#include <stdbool.h>
typedef struct {
    int cpu_fd;
    int memory_fd;
    int io_fd;
} L1LinuxPsiReader;

bool l1_linux_psi_reader_open(L1LinuxPsiReader *reader);
bool l1_linux_psi_reader_read(L1LinuxPsiReader *reader, L1Observation *out,
                              L1PressureCounters *counters);
void l1_linux_psi_reader_close(L1LinuxPsiReader *reader);

#ifdef L1_TESTING
bool l1_linux_psi_reader_open_paths(L1LinuxPsiReader *reader,
                                    const char *cpu_path,
                                    const char *memory_path,
                                    const char *io_path);
bool l1_read_linux_sample_paths(const char *cpu_path, const char *memory_path,
                                const char *io_path, L1Observation *out,
                                L1PressureCounters *counters);
#endif

#endif
