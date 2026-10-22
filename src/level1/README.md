# HMI Lab | Level 1 v0.4 (C11)

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
ssh -p 2222 lab@127.0.0.1 "cd /home/lab/hmi/src/level1 && make clean all test replay bench"
ssh -p 2222 lab@127.0.0.1 "mkdir -p /home/lab/hmi/state && cd /home/lab/hmi/src/level1 && ./l1_monitor --samples 8 --interval-ms 1000 --policy policy.example --state /home/lab/hmi/state/l1.l1state"
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
state. `l1_step_at()` has no kernel interfaces and performs no allocations; its
caller supplies monotonic milliseconds as explicit deterministic input.
An L2-style actor may propose **only** a new policy file in the guest; the L1
process parses the complete candidate and calls `l1_apply_policy()` only if:

1. schema version is 1;
2. generation is strictly increasing (replays rejected);
3. all six named fields are present exactly once, with no unknown keys;
4. values are inside the **compiled** safety envelope;
5. the engine remains single threaded and activates a whole validated policy
   at a decision boundary, resetting to HOLD until observations re-confirm.

The core also retains one prior validated policy. `l1_rollback_policy()` can
restore its behavior only under a caller-supplied generation greater than the
active generation. Rollback therefore does not weaken replay protection and,
like an ordinary update, resets the recommendation to `HOLD`.

This is a *prototype policy contract*, **not** an authenticated L2 security
boundary. Future L2 integration will require access controls, authenticated
origin, authorization, auditing and deeper rollback policies. L2 cannot recompile or
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
range **0..2**. An adjustment requires both **600 valid observations** and
**600,000 elapsed monotonic milliseconds**, then changes by at most one step:
a window with >=8 confirmed recommendation switches adds an extra confirmation;
a completely stable window removes one. Rapid calls cannot accelerate this.
Missing telemetry or regressing time fails to `HOLD`. This conservative anti-flapping
heuristic — it does not imply that L1 has learned an optimal scheduling policy.

## Versioned local state

`--state FILE` opts into persistence of the validated active policy, one prior
policy, and `debounce_extra`. The 64-byte versioned file has a CRC32 and strict
reserved fields. Saving uses a mode-0600 same-directory temporary file, file
`fsync`, atomic rename, and parent-directory `fsync`. Loading any invalid state
recovers compiled defaults and `HOLD`. Actions, timestamps, candidates, and
counters never persist, so restart cannot shortcut the time gate.

This local file is the only new write and is behavior-memory durability—not an
OS/resource control. CRC32 is not authentication. Keep the state outside the
disposable synchronized `src/` tree. See `docs/L1-v0.3-memory-state.md`.

## Acceptance tests and benchmarks

- `make test` — exact synthetic CPU/memory/I/O decision traces plus policy
  parsing, versions, rejection/state preservation, fallback, rollback, bounded
  time-gated adaptation, state integrity/recovery, benchmark argument limits,
  hysteresis, priority, and PSI parser tests.
- `make test-ndebug` — rebuild and rerun checks with `-DNDEBUG`.
- `make sanitize` — ASan/UBSan/leak-enabled test build.
- `make replay` — five independent deterministic fixed-memory/adaptive
  experiments measuring false responses, misses, oscillation, detection delay,
  core size, process RSS, and CPU cost.
- `make bench` — five independent repetitions for fixed-rule and L1 engines.
  `core` runs isolate nanosecond decision cost. `live` runs include all three
  `/proc/pressure` reads and parsing through the decision, and report wall time,
  process CPU time, CPU ns/sample, valid observation count, and Linux peak RSS
  (`ru_maxrss`, KiB). A live run fails if any observation is invalid.
- Custom repeatable run: `sh ./run_benchmarks.sh REPEATS LIVE_SAMPLES CORE_SAMPLES`.
- `make evaluation-ci` — bounded deterministic development, training, and
  one-repetition held-out regression.
- `make evaluation-train EVAL_POLICY=/tmp/l1-v04-tuned.eval.policy` — search
  only the frozen training split.
- `make evaluation-heldout EVAL_POLICY=/tmp/l1-v04-tuned.eval.policy EVAL_REPETITIONS=100`
  — compare frozen default, tuned fixed, and bounded adaptive through the real
  core on the frozen held-out split.
- Observe `memory_gen` and `self_extra` in `l1_monitor` output.
- Program must run as `lab`, without `sudo`.

**Important:** the live benchmark measures observer/controller overhead, not
energy, scheduling improvement, or workload benefit. PSI values and host/guest
load can vary, so compare repeated distributions rather than one run. Peak RSS
has page-sized resolution and includes the whole process. To prove benefits we
still need controlled workloads, independent outcomes, and net cost accounting.

The preregistered v0.4 held-out experiment was a negative result: tuned fixed
memory outperformed bounded adaptation, and adaptation regressed memory-event
misses and maximum detection delay. It is explicitly a no-go for actuation.
See `docs/L1-v0.4-evaluation-plan.md` and `docs/L1-v0.4-validation.md`.

## Components

```
l1.h             Fixed memory/policy/observation interface
l1_core.c        Deterministic memory-based decision engine + slow self-change
l1_linux.c       Read-only Linux PSI adapter
l1_policy.c      Strict L2-style behavior-memory proposal parser
l1_state.c       Versioned CRC state, atomic persistence and safe recovery
l1_main.c        Monitor, monotonic clock adapter, policy/state loop
l1_test.c        Policy, fallback, adaptation, parser and rollback tests
l1_trace_test.c  Synthetic traces with known per-sample decisions
l1_state_test.c  Persistence corruption/recovery/permission tests
l1_bench.c       Fixed-vs-L1 core and observation-to-decision benchmark
l1_replay.c      Labeled fixed-memory vs bounded-adaptive experiment
evaluation/l1_evaluate.c  Seeded train/held-out three-controller evaluator
evaluation/run_ci.sh  Bounded CI evaluation command
evaluation_args_test.sh  Evaluator CLI, checksum, and training-selection tests
benchmark_args_test.sh  Benchmark CLI negative tests
run_benchmarks.sh Repeated guest benchmark command
run_replays.sh   Repeated deterministic replay command
policy.example   Valid initial policy proposal
policy.invalid.example  Negative policy test fixture
Makefile         Build/test/sanitize/replay/benchmark
```
