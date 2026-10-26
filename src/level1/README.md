# HMI Lab | Level 1 v1.0 candidate (C11)

**First working implementation — observe / decide / recommend ONLY.**
It does not alter process priorities, cgroups, CPU registers, RAM, kernel state,
firmware, QEMU, or the Windows host. This is **not yet a trained AI** and has
not demonstrated resource-management improvements. It is a safe research
baseline with a tiny memory-parametrized controller and outcome-aware shadow
forecast experiment.

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
values are the kernel's short-term averages. V0.6 also reads their cumulative
`total` counters for delayed shadow outcomes. Missing telemetry forces `HOLD`.

Possible recommendations:

- `HOLD` — no policy suggestion.
- `PREFER_INTERACTIVE` — CPU stall pressure crossed the validated threshold.
- `PROTECT_MEMORY` — memory *full* stall pressure crossed its threshold.
- `DEFER_IO` — I/O *full* stall pressure crossed its threshold.

Recommendations do **nothing** to the OS in this release.

## R2.1 descriptive perception lane

`l1_perception_classify()` is a separate, pure C11 classifier for a copied
`L1Observation`. It describes the current PSI stall sample as `UNKNOWN`,
`QUIET`, one of the three single-resource pressure states, `MIXED_PRESSURE`,
or `UNRELIABLE`. It has no clock, heap, I/O, global state, policy access,
learning state or action authority.

The fixed descriptive boundaries are CPU `some >= 3000 bp`, memory
`full >= 100 bp`, and I/O `full >= 500 bp`. These are immutable R2.1
descriptors, not CPU/RAM utilization, device health, anomaly detection, or a
replacement for the protected reflex policy. `QUIET` only means none of those
three PSI boundaries was reached. Policy generation, confirmation and prior
core history can legitimately make the protected action differ from the
description; the published advisory must never be erased or changed by
perception.

Input quality is explicit caller metadata. The stateless classifier cannot
infer a missing observation, stale sample, clock regression or counter reset
from one observation. `ABSENT` produces `UNKNOWN`; invalid, stale, regressed,
reset, malformed or out-of-range input produces `UNRELIABLE`. Until a future
separately verified adapter exists, that quality label must not be treated as
independently authenticated evidence.

R2.1 contains no novelty score, confidence estimate, machine individuality,
learned baseline, forecast, action coupling or resource optimization. Run its
known-answer and isolation tests with `./l1_perception_test`; run the isolated
five-repetition cost/RSS measurement with `make perception-bench`.

## Memory-driven behavior / safe policy proposal

`L1Core` contains a compact `L1Policy` (behavior memory) and bounded working
state. `l1_step_at()` has no kernel interfaces and performs no allocations; its
caller supplies monotonic milliseconds as explicit deterministic input.
The legacy development monitor may read a new policy file in the guest; the L1
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
boundary. The `--policy` route is explicitly an unauthenticated legacy
development interface and cannot be used as a production L2 authorization
path. Future integration requires an approved trust anchor, protected key
lifecycle, independent monotonic replay storage, access control and auditing.
L2 cannot recompile or change L1's immutable safety rules *through this
contract*.

V1.1 adds a separate `l1_proposal_check` offline tool for the canonical
`HMI_L1_POLICY_PROPOSAL_V1` envelope. It performs strict bounded parsing,
compiled-policy checks, compare-and-swap generation checks, and best-effort
local file safeguards. It always reports `authority=none` and
`production_apply=blocked`; no command-line flag, source label or envelope
field can authorize application. Only a binary built with `L1_TESTING` has
the mock verifier seam, and successful tests apply solely to a copied
`L1Core`. That mock compares exact bytes but is not cryptographic
authentication and is not present as a positive production provider.

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

V0.5 maintains independent CPU and I/O confirmation extras, each in **0..2**.
Memory escalation has an independent streak and always uses the validated
policy's base confirmation count: adaptation cannot add memory-entry delay.
An adjustment requires both **600 valid observations** and **600,000 elapsed
monotonic milliseconds**, then changes by at most one step. Rapid calls cannot
accelerate it. Missing telemetry or regressing time fails to `HOLD`.

The V0.4 global heuristic remains compiled only as a reproducible research
comparator. This is bounded anti-flapping logic, not online ground-truth
learning or evidence that L1 has learned an optimal resource policy.

## Outcome-aware shadow forecast

V0.6 makes a binary future-pressure prediction from information available at
the current sample, then evaluates it only after a 60-second monotonic window
using cumulative PSI counter differences. Its three threshold offsets have
compiled bounds, require at least 600 valid observations and 600,000 ms, and
move by at most one step after repeated directional errors. Counter reset,
overflow, invalid input, long gaps, or time regression discards the pending
evidence safely.

`L1Shadow` is fixed-size, allocation-free, and isolated from `L1Core`. Shadow
predictions and learned offsets cannot modify a recommendation or V0.5 safety
behavior. They are not persisted. The held-out V0.6 experiment made bounded
updates but did not improve any forecast over tuned static, so its scientific
result and action-coupling decision are both NO-GO.

## Versioned local state

`--state FILE` opts into persistence of the validated active policy, one prior
policy, and the CPU/I/O extras. The 72-byte v2 file has a CRC32 and strict
reserved fields. A fully validated 64-byte v1 file migrates policy and rollback
memory, discards its global extra, and starts both V0.5 extras at zero. Saving
uses a mode-0600 same-directory temporary file, file
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
- `make perception-bench` — five optimized measurements of the stateless R2.1
  classifier, reporting per-run and min/median/max process CPU cost per call,
  process RSS, result size and zero persistent classifier state.
- `make proposal-ci` — canonical/adversarial parsing, default-deny
  authorization, filesystem replacement, byte-isolation, HOLD reset and
  rollback tests for the offline V1.1 boundary.
- `make proposal-bench` — five optimized process measurements covering one
  canonical envelope plus one maximum-size negative envelope.
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
- `make evaluation-v05-ci` — bounded V0.5 development, training, manifest
  identity, and held-out smoke test.
- `make evaluation-v05-heldout EVAL_REPETITIONS=100` — preregistered V0.5
  four-controller held-out comparison.
- `make evaluation-v06-ci` — bounded outcome-feedback development, training,
  manifest-identity, and sanitizer-compatible held-out smoke test.
- `make evaluation-v06-heldout EVAL_REPETITIONS=100` — preregistered V0.6
  forecast and unchanged-action comparison.
- `make shadow-bench` — repeated shadow hot-path and complete Linux
  observation-path timing/RSS measurement.
- Observe `memory_gen`, `self_cpu_extra`, `self_io_extra`, `shadow_mask`,
  feedback status, and shadow offsets in `l1_monitor` output.
- Program must run as `lab`, without `sudo`.

**Important:** the live benchmark measures observer/controller overhead, not
energy, scheduling improvement, or workload benefit. PSI values and host/guest
load can vary, so compare repeated distributions rather than one run. Peak RSS
has page-sized resolution and includes the whole process. To prove benefits we
still need controlled workloads, independent outcomes, and net cost accounting.

The preregistered V0.5 held-out experiment was also a negative result.
Risk-aware adaptation eliminated extra memory delay and improved on V0.4 global
adaptation, but regressed I/O misses and total loss versus tuned fixed. It is
explicitly a no-go for actuation. See `docs/L1-v0.5-evaluation-plan.md` and
`docs/L1-v0.5-validation.md`.

The V0.6 held-out result is also negative: the shadow learner's forecasts tied
the tuned static predictor on every held-out scenario. See
`docs/L1-v0.6-evaluation-plan.md` and `docs/L1-v0.6-validation.md`.

## Bounded PSI recorder efficiency

V0.8 keeps three read-only PSI descriptors open for the lifetime of an
explicit finite recording and uses bounded `pread()` buffers for each sample.
This removes repeated procfs open/close work without caching observations,
skipping reads, changing the V1 trace schema, or weakening `fsync` and atomic
no-replace publication. Phase-boundary accounting reports setup, capture-loop,
publication, total process CPU, wall time, and RSS separately.

The retained V0.7 one-shot comparator preserves its historical `fgets()`
behavior, including acceptance of a syntactically complete final fixture line
without a newline. The production persistent reader is deliberately stricter:
Linux PSI records must be newline-terminated, fit in its bounded buffer, and
are isolated before parsing so fields cannot be borrowed from a later line.
Both readers are value-equivalent for supported newline-terminated Linux PSI
input; EOF-without-newline is retained only as baseline compatibility behavior.

`make recorder-bench` runs the frozen five-repetition synthetic 5/65/180-row
comparison between the retained one-shot reader and persistent reader. It uses
the V1 trace encoder, checksum, strict parser, durability, and publication path
but does not sleep or record real PSI. `make psi-bench` compares hot read/parse
cost against live `/proc/pressure/*` without retaining observations. Neither
command generates workload or measures resource-management benefit.

A real trace longer than a few-sample smoke capture still requires explicit
user authorization naming its finite duration and guest-local destination.
Raw traces remain ignored and are never uploaded automatically. See
`docs/L1-v0.8-evaluation-plan.md` and `docs/L1-v0.8-validation.md`.

## Transparent replay and human inspection

V0.9 adds l1_inspect, a bounded read-only viewer that calls the existing
l1_step_at() and l1_shadow_step_at() implementations directly. It never
publishes an action other than the core's returned advisory action and never
feeds viewer or shadow output back into the core. The independent reference
runner verifies the action and all L1Core bytes after every row.

    ./l1_inspect demo
    ./l1_inspect demo overlap-priority --details 60
    ./l1_inspect demo policy-rollback
    ./l1_inspect trace /absolute/guest/path/example.l1trace \
      --forecast-config live-default --details 20
    ./l1_inspect trace /absolute/guest/path/example.l1trace \
      --forecast-config v06-trained --details 20

The threshold_hits field is an observable comparison against the displayed
behavior memory, while reason is a bounded diagnostic summary of actual
before/after core state. It is not a rewritten decision engine. Published and
fixed actions are produced by separate real core instances; shadow_prediction
is explicitly non-actionable. Trace mode validates the complete V1 file before
replay, buffers at most 2,000 detail records, and verifies file identity and
checksum again before output.

make inspect-ci runs fixed action/checksum references, golden output, argument
and equivalence tests. make inspect-bench generates a private synthetic
maximum-row trace in /tmp, measures parser, core+shadow, formatting, complete
CLI CPU and RSS, and removes the trace. It does not read PSI or create load.
See docs/L1-human-acceptance.md.

## Offline real-PSI evidence analyzer

V1.0 adds `l1_evidence`, a finite read-only analyzer for an already-existing,
fully validated V1 PSI trace. It performs a complete validation pass before
analysis, then verifies file identity, row count, byte count, and checksum on
the replay pass. It never prints the private input path or raw samples.

    ./l1_evidence analyze /absolute/guest/data/session.l1trace \
      --source real-guest-v1

The analyzer freezes the first 60% by relative monotonic time as development,
skips a 65-second embargo, and initializes independent learner and core state
for the final evaluation segment. It separately compares static and unchanged
adaptive forecasts for `live-default=800/40/150` and
`v06-trained=2000/75/600`. Published actions come from the existing
`l1_step_at()` and are compared byte-for-byte with an observer-free reference;
the `--source` value is only a caller declaration. Numerical coverage is
reported separately, and the CLI cannot establish capture authorization or
independent provenance. A caller-declared real trace therefore remains
`REAL_EVIDENCE:INSUFFICIENT` until provenance is reviewed outside the tool.
forecasts come only from `l1_fixed_prediction()` and `l1_shadow_step_at()`.

`make evidence-ci` generates bounded synthetic fixtures for split, feedback,
gap, reset, invalid-input, underpopulation, accounting, golden-output, and
privacy checks. `make evidence-bench` analyzes an 86,400-row synthetic trace
five times and reports complete process CPU/wall time and RSS. These fixtures
validate tooling, not real forecast transfer. A research-length capture still
requires a separate user instruction containing both duration and guest-local
output path, and raw recordings must not enter this repository.

## Components

```
l1.h             Fixed memory/policy/observation interface
l1_core.c        Deterministic memory-based decision engine + slow self-change
l1_linux.c       Read-only Linux PSI adapter
l1_linux.h       Bounded persistent read-only PSI reader interface
l1_linux_test.c  One-shot/persistent PSI equivalence and failure tests
l1_shadow.h/.c   Fixed-size delayed-outcome shadow learner
l1_perception.h/.c Pure stateless R2.1 PSI condition description
l1_perception_test.c Frozen masks, quality, boundaries and core isolation
l1_perception_bench.c Bounded classifier CPU/p95/RSS measurement
l1_policy.c      Strict L2-style behavior-memory proposal parser
l1_proposal.h/.c V1.1 canonical offline proposal boundary (default deny)
l1_proposal_check.c Offline validity CLI; no production apply authority
l1_proposal_test.c Test-only exact-byte mock verifier and clone safety tests
l1_state.c       Versioned CRC state, atomic persistence and safe recovery
l1_main.c        Monitor, monotonic clock adapter, policy/state loop
l1_test.c        Policy, fallback, adaptation, parser and rollback tests
l1_trace_test.c  Synthetic traces with known per-sample decisions
l1_state_test.c  Persistence corruption/recovery/permission tests
l1_shadow_test.c Causal feedback, reset/saturation, unchanged-action tests
l1_bench.c       Fixed-vs-L1 core and observation-to-decision benchmark
l1_shadow_bench.c Shadow hot-path and complete observation benchmark
l1_recorder_bench.c Deterministic complete recorder-path comparison
l1_psi_bench.c   Non-recording live PSI read/parse profiler
l1_demo.h/.c     Frozen seeded transparent-replay demonstration rows
l1_inspect.c     Read-only action/shadow/fixed-controller replay viewer
l1_inspect_test.c Independent expected-action and core-byte equivalence tests
l1_inspect_bench.c Bounded parser/replay/format/CLI resource measurement
l1_evidence.c    Validated split/scoring/readiness observer for V1 traces
l1_evidence_bench.c Complete analyzer process CPU/wall/RSS measurement
l1_replay.c      Labeled fixed-memory vs bounded-adaptive experiment
evaluation/l1_evaluate.c  Seeded train/held-out three-controller evaluator
evaluation/l1_evaluate_v05.c  Frozen four-controller risk-aware evaluator
evaluation/l1_evaluate_v06.c  Frozen forecast/action shadow evaluator
evaluation/run_ci.sh  Bounded CI evaluation command
evaluation/run_ci_v05.sh  Bounded V0.5 CI evaluation command
evaluation/run_ci_v06.sh  Bounded V0.6 CI evaluation command
evaluation_args_test.sh  Evaluator CLI, checksum, and training-selection tests
evaluation_v05_args_test.sh  V0.5 checksum and manifest-identity tests
evaluation_v06_args_test.sh  V0.6 checksum and manifest-identity tests
benchmark_args_test.sh  Benchmark CLI negative tests
run_benchmarks.sh Repeated guest benchmark command
run_replays.sh   Repeated deterministic replay command
run_recorder_benchmarks.sh Repeated synthetic recorder benchmark
run_psi_benchmarks.sh Repeated live PSI source-only profile
policy.example   Valid initial policy proposal
policy.invalid.example  Negative policy test fixture
Makefile         Build/test/sanitize/replay/benchmark
```
