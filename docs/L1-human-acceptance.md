# HMI Level 1 human acceptance guide

## Scope and safety

This packet is for personal review of the V0.9 read-only release candidate.
L1 reports advisory actions; it does not apply them. The viewer does not write
policies, state, traces, cgroups, scheduler settings, devices, firmware, or
hardware. It does not generate CPU, memory, or I/O pressure.

The synthetic demonstrations are evidence that the existing compiled core is
being replayed transparently. They are not evidence of resource savings or a
successful learning algorithm. Historical scientific conclusions remain:

- V0.8 recorder efficiency: GO; real evidence: INSUFFICIENT.
- V0.7 diagnostics: NO_GO.
- V0.6 shadow prediction and action coupling: NO_GO.
- adaptation: NO_GO.
- real resource benefit: NOT_ESTABLISHED.

## Reproduce from Windows and Debian QEMU

From PowerShell in C:\Dev\Projects\HMI-Lab, confirm the VM is already running
and deploy the current source:

    git switch feature/l1-v0.9-transparent-replay
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sync-source.ps1
    ssh -p 2222 lab@127.0.0.1

Inside the Debian guest:

    cd /home/lab/hmi/src/level1
    make clean all test
    ./l1_inspect help
    ./l1_inspect demo --details 0

No Windows configuration, service, device, firmware, scheduler, or resource
setting is changed by these commands.

## Review the demonstrations

Run each detailed scenario:

    ./l1_inspect demo idle-noise
    ./l1_inspect demo rising-cpu
    ./l1_inspect demo urgent-memory
    ./l1_inspect demo sustained-io
    ./l1_inspect demo overlap-priority
    ./l1_inspect demo invalid-telemetry
    ./l1_inspect demo monotonic-regression
    ./l1_inspect demo reset-recovery
    ./l1_inspect demo policy-rollback
    ./l1_inspect demo shadow-prediction

Read each row as follows:

- cpu_bp, memory_bp, and io_bp are observed basis points.
- threshold_hits is a direct comparison against the displayed policy memory;
  it is context, not a duplicate decision engine.
- published is the advisory action returned by l1_step_at().
- fixed is a separate l1_step_at() instance with adaptation disabled.
- candidate and streak fields expose actual confirmation state.
- reason summarizes actual before/after core state and explicitly names invalid
  or regressing-time fallback.
- static_live is the compiled 800/40/150 fixed forecast.
- static_v06 is the historical 2000/75/600 fixed forecast.
- shadow_prediction and feedback come only from l1_shadow_step_at(). They never
  change published.
- observer_equivalence must be PASS and observer_mismatches must be zero.

Expected safety landmarks:

- rising-cpu recommends PREFER_INTERACTIVE only after confirmation.
- urgent-memory recommends PROTECT_MEMORY.
- sustained-io recommends DEFER_IO.
- overlap-priority progresses through CPU, I/O, then memory priority.
- invalid and regressing telemetry immediately publish HOLD.
- policy-rollback accepts generation 1, rejects an out-of-bounds memory
  threshold, then rolls back default behavior as generation 2.
- shadow-prediction reports evaluated feedback separately while published
  remains an action from the core.

## Persistence, corruption, and restart evidence

The viewer never creates or changes persistent state. Exercise the independently
tested V1/V2 migration, restart, CRC corruption, permission, atomic replacement,
and recovery paths with:

    ./l1_state_test
    ./l1_test

Expected final lines begin with PASS. A corrupt or unsupported state must fall
back safely; it must not be silently reinterpreted. V2 remains 72 bytes.

## Optional private V1 trace

Only use an already-recorded guest-local V1 trace that you explicitly choose.
Do not copy private traces into the source tree or upload them.

    ./l1_inspect trace /absolute/guest/path/example.l1trace \
      --forecast-config live-default --details 20
    ./l1_inspect trace /absolute/guest/path/example.l1trace \
      --forecast-config v06-trained --details 20

The tool rejects relative paths, parent-directory components, non-regular
files, malformed data, bad checksums, excessive rows/bytes, and files that
change between validation and replay. It prints no rows until both bounded
passes succeed. FNV-1a detects accidental corruption; it is not authentication.

## Engineering and resource checks

    make test-ndebug
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 make sanitize
    make clean all replay
    make evaluation-ci evaluation-v05-ci evaluation-v06-ci evaluation-v07-ci
    make recorder-bench-ci
    make inspect-bench

The full inspect benchmark creates only a synthetic temporary V1 trace under
/tmp and removes it on normal exit or signals. The optimized CLI RSS acceptance
limit is 8,192 KiB. Parser, core-plus-shadow, formatting, complete CLI CPU, and
CLI RSS are reported separately. Sanitizer timing and RSS are correctness
evidence, not optimized-budget evidence.

## Personal scorecard

Record one of clear, unclear, or unacceptable for each item:

| Question | Rating | Notes |
|---|---|---|
| Can you identify the published action on every row? | | |
| Are fallback and pending confirmation understandable? | | |
| Is memory over I/O over CPU priority visible? | | |
| Is shadow prediction clearly separate from action? | | |
| Is fixed-controller comparison understandable? | | |
| Is policy rejection and rollback understandable? | | |
| Does output remain readable at the chosen detail limit? | | |
| Are limitations and missing real-benefit evidence explicit? | | |

Proposed behavior changes should name the scenario, row, observed values,
current published action, desired advisory action, and safety rationale. A
request does not authorize changing compiled priority, actuation, L2/L3, held-
out data, or historical findings.

## Stop and recovery

The viewer is finite and foreground-only. Press Ctrl+C to stop it. It launches
no service and leaves no background worker. The benchmark installs a cleanup
trap for its synthetic /tmp files. If any command reports FAIL, preserve the
output, stop acceptance review, and do not treat the candidate as ready.
