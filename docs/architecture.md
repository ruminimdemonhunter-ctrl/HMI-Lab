# HMI Lab design boundaries

- **Windows host**: VS Code, Codex, Git, QEMU binary, source of truth, offline VM backups.
- **Debian guest**: isolated experimental testbed with SSH server and deployed source copy.
- **L1 future research**: local low-cost control law/model; emulated hardware and Linux telemetry only. Separate a safety guard from an adaptive policy.
- **L2 future research**: explicit OS resource policy orchestration, not an LLM in the kernel.
- **L3 future research**: occasional strategic reasoning (on-device or cloud), advisory only.
- **Core rule**: high-level AI suggests goals; lower-level policies enforce limits. Conventional OS and hardware safety rules always win.
- **Trust boundary**: no raw physical disk passthrough, host filesystem mount, privileged host device control, or bridged network. QEMU guest internet is permitted only for installation/explicit package-setup sessions; day-to-day VM start uses QEMU `restrict=on`, with the explicit Windows loopback SSH forwarding still allowed.
- **Data flow**: `Windows src/` --(SCP over host loopback port 2222)--> `/home/lab/hmi/src/`. Sync destroys and replaces the guest source tree, but preserves guest OS and data outside this tree. VM changes are not synced back.
- **Testing**: compare any L1 candidate to built-in Linux policies with overhead included; evaluate throughput, interactivity, memory pressure, and control stability.

## Level 1 v0.2 validation boundary

Level 1 remains a native C11, single-threaded, read-only recommendation engine.
Its compiled priority order, validation ranges, hysteresis, and fallback logic
are not policy-writable. Behavior memory contains only fixed-width thresholds,
confirmation count, schema version, and generation. A rejected update leaves the
entire core state unchanged.

The core retains one previously accepted policy snapshot. Rollback copies that
snapshot into a new, strictly greater generation and resets runtime state to
`HOLD`; generations never move backward, so rollback cannot bypass replay
protection. This is process-local behavior only, not a durable or authenticated
policy store.

Validation is split deliberately:

- deterministic synthetic traces verify CPU, memory, and I/O decisions without
  depending on current guest load;
- policy tests verify versions, malformed updates, immutable bounds, fail-safe
  state, slow adaptation, and rollback;
- core microbenchmarks isolate decision cost;
- separate Linux PSI observation-to-decision runs report process CPU time and
  peak RSS against a fixed-rule comparator.

Neither benchmark measures resource-management benefit: v0.2 has no actuator
and makes no resource-control, scheduler, kernel, firmware, or hardware writes.

## Level 1 v0.3 memory stability boundary

The deterministic core receives monotonic time as explicit data; it does not
read a clock itself. Self-adaptation can change only `debounce_extra` in `0..2`,
and only after both 600 valid observations and 600,000 elapsed monotonic
milliseconds. Call frequency therefore cannot accelerate it. Time regression,
invalid telemetry, update, rollback, and recovery all return or reset to `HOLD`.

Optional local persistence stores only validated policy memory, one rollback
snapshot, and the bounded debounce increment. It never restores actions,
candidate streaks, decision counters, or time-window state. Its CRC is corruption
detection, not authentication. See `L1-v0.3-memory-state.md`.

The v0.3 replay gives the heuristic a labeled synthetic trace and measures
false responses, oscillation, and detection delay. Better scores on that trace
are experimental evidence about that heuristic/trace pair—not machine learning,
general intelligence, workload benefit, or permission to actuate resources.

## Level 1 v0.4 adaptive-evaluation boundary

V0.4 adds a read-only experimental harness, not a new intelligence layer. A
seeded generator keeps latent event truth independent from noisy observations,
and separate development, training, and held-out seeds prevent parameter
selection on the reported test scenarios. Frozen default, training-tuned fixed,
and bounded-adaptive candidates all call the same compiled l1_step_at() path
with identical observations and timestamps.

The only new core control enables or disables the already compiled adaptation
heuristic for comparison. It defaults enabled, is not policy-file writable or
persisted, and cannot change priority, thresholds, bounds, fallback behavior,
or executable logic.

The held-out result was unfavorable: tuned fixed memory beat bounded adaptation
on the declared loss, and adaptation regressed the high-priority memory metric.
The preregistered outcome is therefore no-go for actuation. See
L1-v0.4-evaluation-plan.md and L1-v0.4-validation.md. This result adds no
scheduler, cgroup, kernel, device, network update, or hardware authority.

## Level 1 v0.5 risk-aware memory boundary

V0.5 replaces production-default global debounce adaptation with independently
bounded CPU and I/O extras. Memory escalation uses a separate confirmation
streak and exactly the tuned fixed policy's base confirmation count, so
adaptive state cannot add memory-protection entry delay. Each extra remains in
`0..2` and can change by only one after both 600 valid observations and 600,000
monotonic milliseconds. The compiled memory > I/O > CPU > HOLD priority is
unchanged.

Persistent behavior memory is now explicit version 2. A validated version-1
file migrates policy and rollback memory only; its global adaptive extra is
discarded instead of being silently reinterpreted. Runtime actions, timing,
counters, and partial confirmations remain nonpersistent.

The preregistered held-out result is again no-go. V0.5 matched tuned fixed on
all paired memory-event delays and improved on V0.4 global loss, but regressed
I/O misses and total loss relative to tuned fixed. See
`L1-v0.5-evaluation-plan.md` and `L1-v0.5-validation.md`. This is bounded
heuristic adaptation, not online learning or evidence of general intelligence.

## Level 1 v0.6 outcome-feedback boundary

V0.6 adds a fixed-size shadow forecaster beside, not inside, the published
decision path. The Linux adapter reads cumulative PSI totals for CPU `some`,
memory `full`, and I/O `full`; clock and filesystem access remain outside the
deterministic components. A forecast is scored only after its monotonic future
window has elapsed. Invalid input, gaps, counter decreases, overflow, and time
regression discard evidence rather than creating an outcome.

The learner may adjust only three forecast-threshold offsets inside compiled
bounds after both an observation/time gate and repeated directional errors.
It has no pointer or API capable of changing `L1Core`, behavior memory,
priority, fallback, or published actions. It uses fixed storage and no hot-path
allocation. V1/V2 behavior-state encoding is unchanged and shadow state is not
persisted.

The frozen held-out result is no-go: bounded updates occurred, but every
forecast remained identical to the tuned static predictor and Brier
improvement was 0%. V0.6 therefore remains read-only research and is not
eligible for action coupling. See `L1-v0.6-evaluation-plan.md` and
`L1-v0.6-validation.md`.

## Level 1 v0.7 workload-evidence boundary

V0.7 adds evidence capture and diagnostics, not another learner. An explicitly
invoked Debian recorder reads existing PSI files and writes a finite,
checksummed, mode-0600 trace only to a caller-selected guest-local path outside
synchronized source. Its schema excludes wall time, identity, process, command,
network, and credential data. It never starts a workload, uploads data, writes
PSI triggers, or changes resource controls.

The new synthetic generator consumes a dedicated truth stream so event class,
timing, duration, overlaps, regime boundaries, and faults vary across seeds.
Streaming evaluation compares both the live `800/40/150` and historical
`2000/75/600` forecast thresholds while keeping the V0.6 learner, L1 decisions,
and V1/V2 persistence unchanged. Offline truth is unavailable to runtime
learning.

The diagnostic machinery passed its synthetic, memory, and unchanged-action
gates, but the recorder exceeded its frozen CPU/sample budget. V0.7 is therefore
`DIAGNOSTICS_NO_GO`; V0.6 prediction and action coupling remain NO-GO. See
`L1-v0.7-evaluation-plan.md` and `L1-v0.7-validation.md`.

## Level 1 v0.8 recorder-efficiency boundary

V0.8 changes only the explicitly invoked PSI recorder's observation adapter.
It opens the same CPU `some`, memory `full`, and I/O `full` procfs sources once,
reads every source on every cadence through bounded `pread()` buffers, and
closes all descriptors before publication. It does not cache or infer a sample,
write a PSI trigger, run a workload, or change L1 decisions or shadow learning.

The V1 trace bytes and strict parser, finite sample/duration/file caps,
mode-0600 temporary file, `fsync`, atomic no-replace hard-link publication,
signal cleanup, decision core, shadow state, and 72-byte V2 persistence remain
unchanged. Performance tooling uses deterministic synthetic PSI fixtures and
separately profiles live read/parse without retaining observations. Longer real
recordings remain opt-in and guest-local.

Recorder efficiency and real-evidence readiness have separate preregistered
verdicts. Neither can revise the historical `DIAGNOSTICS`,
`SHADOW_PREDICTION`, or `ACTION_COUPLING` NO-GO findings or authorize resource
actuation. See `L1-v0.8-evaluation-plan.md` and `L1-v0.8-validation.md`.
