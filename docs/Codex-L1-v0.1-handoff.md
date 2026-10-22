# Codex handoff | HMI Lab Level 1 v0.1

Edit the Windows source workspace at `C:\Dev\Projects\HMI-Lab`.
The Debian VM is a disposable runtime, **not** the source of truth.

## Immediate implementation target

Review the existing `src/level1` native C11 prototype for correctness,
security boundaries, compiler portability on Debian 13, and test quality.
Do not rewrite the three-level architecture or replace C with Python/Rust.

### Invariants

1. L1 core logic and safety limits are compiled, deterministic, and bounded.
2. All behavior comes from validated compact memory/policy parameters.
3. L1 adjusts only restricted `debounce_extra` at a slow interval.
4. L2 (not yet built) may only propose L1 memory changes through a validated
   versioned contract; no code injection/replacement or arbitrary addresses.
5. L3 (not yet built) adapts to the user, passes intentions to L2; do not
   implement L3 yet.
6. v0.1 is OBSERVE/RECOMMEND only; no real kernel, cgroup, process priorities,
   thermal, firmware, host settings, VM config, or filesystem changes beyond
   the controlled guest source/build files.
7. No dependencies beyond C11/libc and Debian `build-essential`.
8. Keep every binary and work product inside the guest; Windows sources only.

### Validation steps

- Run `make clean all test bench` in the Debian VM directory.
- Exercise `./l1_monitor --samples 8 --policy policy.example`.
- Check malformed, duplicated, missing, stale and range-violating memory
  proposals preserve the active validated behavior memory.
- Check telemetry failures immediately produce `HOLD`.
- Add regression tests before making algorithm changes.
- Benchmark full process overhead separately from core-only inference.
- Record the actual Debian outputs, compile warnings, tests, and limitations.

### Done criteria

A reproducible Debian run with all tests passing, documented output, measured
core-only overhead, and no privileged writes. After that, propose the next
controlled benchmark experiment. Do not declare L1 production-ready.
