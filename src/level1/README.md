# HMI Lab | Level 1 v0.1 (C11)

**First working implementation — observe / decide / recommend ONLY.**
It does not alter process priorities, cgroups, CPU registers, RAM, kernel state,
firmware, QEMU, or the Windows host. This is **not yet a trained AI** and has
not demonstrated resource-management improvements. It is a safe research
baseline with a tiny memory-parametrized adaptive controller.

## Where it runs

- All source is edited on Windows in `C:\Dev\Projects\HMI-Lab\src\level1`.
- Deploy Windows source to Debian with the existing `scripts\sync-source.ps1`.
- Build and execute from Debian as unprivileged user `lab`.
- Dependencies: `make`, `gcc`, and libc (provided by `build-essential`).

## On Windows PowerShell (VM must be running in isolated mode)

```powershell
cd C:\Dev\Projects\HMI-Lab
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\sync-source.ps1
ssh -p 2222 lab@127.0.0.1 "cd /home/lab/hmi/src/level1 && make clean all && make test && make bench"
ssh -p 2222 lab@127.0.0.1 "cd /home/lab/hmi/src/level1 && ./l1_monitor --samples 8 --interval-ms 1000 --policy policy.example"
```

The CLI samples `/proc/pressure/cpu` **some** and `/proc/pressure/memory`
and `/proc/pressure/io` **full**; these are Linux PSI *stall* metrics, not
physical CPU/RAM utilization or exclusive ownership of hardware. `avg10`
values are the kernel's short-term averages. Missing telemetry forces `HOLD`.

Possible recommendations:

- `HOLD` — no policy suggestion.
- `PREFER_INTERACTIVE` — CPU stall pressure crossed the validated threshold.
- `PROTECT_MEMORY` — memory *full* stall pressure crossed its threshold.
- `DEFER_IO` — I/O *full* stall pressure crossed its threshold.

Recommendations do **nothing** to the OS in this release.

## Memory-driven behavior / safe policy proposal

`L1Core` contains a compact `L1Policy` (behavior memory) and bounded working
state. `l1_step()` has no kernel interfaces and performs no allocations.
An L2-style actor may propose **only** a new policy file in the guest; the L1
process parses the complete candidate and calls `l1_apply_policy()` only if:

1. schema version is 1;
2. generation is strictly increasing (replays rejected);
3. all six named fields are present exactly once, with no unknown keys;
4. values are inside the **compiled** safety envelope;
5. the engine remains single threaded and activates a whole validated policy
   at a decision boundary, resetting to HOLD until observations re-confirm.

This is a *prototype policy contract*, **not** an authenticated L2 security
boundary. Future L2 integration will require access controls, authenticated
origin, persistence, auditing and rollback policies. L2 cannot recompile or
change L1's immutable safety rules *through this contract*.

The policy file is checked every 5 observation cycles (not at microsecond
scheduling timescales). For a continuous demo, from Debian:

```bash
mkdir -p /home/lab/hmi/policies
cp /home/lab/hmi/src/level1/policy.example /home/lab/hmi/policies/l1.policy
cd /home/lab/hmi/src/level1
./l1_monitor --samples 0 --interval-ms 1000 --policy /home/lab/hmi/policies/l1.policy
```

In a second SSH session, simulate an L2 policy memory update by writing a
**complete new file** and renaming it to the target atomically:

```bash
sed -e 's/generation=1/generation=2/' \
    -e 's/cpu_trigger_bp=3000/cpu_trigger_bp=2500/' \
    /home/lab/hmi/src/level1/policy.example > /home/lab/hmi/policies/l1.policy.next
mv /home/lab/hmi/policies/l1.policy.next /home/lab/hmi/policies/l1.policy
```

Next refresh should print `applied validated behavior memory generation 2`.
This changes **memory parameters**, not the engine's compiled code. The live
policy path is deliberately **outside** `src/` because sync replaces `src/`.

## Restricted self-adaptation

L1 self-modifies just one bounded behavior parameter, `debounce_extra`, in
range **0..2**. After each **600 valid observations**, it can change by **at
most one step**: a window with >=8 confirmed recommendation switches adds
an extra confirmation; a completely stable window reduces one extra
confirmation. At the 1000 ms default sample interval, that is approximately
10 minutes between any adjustments. This is a conservative anti-flapping
heuristic — it does not imply that L1 has learned an optimal scheduling policy.

## Acceptance tests

- `make test` — deterministic decisions, priorities, hysteresis, sensor parser,
  missing telemetry fail-safe, policy generation/rejection, bounded adaptation.
- `make bench` — *in-process core-only* average decision computation cost.
- Observe `memory_gen` and `self_extra` in `l1_monitor` output.
- Program must run as `lab`, without `sudo`.

**Important:** `make bench` does not measure kernel sensor-read overhead,
energy, system speedup, or real hardware effects. To prove benefits we need
controlled workloads, fixed baselines, independent outcome measurements and
net cost accounting.

## Components

```
l1.h             Fixed memory/policy/observation interface
l1_core.c        Deterministic memory-based decision engine + slow self-change
l1_linux.c       Read-only Linux PSI adapter
l1_policy.c      Strict L2-style behavior-memory proposal parser
l1_main.c        Low-overhead monitoring and safe policy-reload loop
l1_test.c        Deterministic unit tests
l1_bench.c       Core-only microbenchmark
policy.example   Valid initial policy proposal
policy.invalid.example  Negative policy test fixture
Makefile         Build/test/benchmark
```
