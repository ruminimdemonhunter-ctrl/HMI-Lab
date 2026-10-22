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
