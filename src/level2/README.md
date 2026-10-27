# HMI Level 2 V0.1 experience core

This directory contains a synthetic-only engineering prototype of the bounded
Level 2 experience structure frozen in `docs/L2-v0.1-evaluation-plan.md`.
It is independent of the protected Level 1 decision engine.

## Boundary

- `l2_state.c` is a pure C11 state transition module. The caller supplies the
  state, event and timestamp; the module has no heap, clock, filesystem,
  network or OS-control access.
- The model stores only fixed enums, bounded counters and opaque workspace
  numbers. `sizeof(L2State)` is checked against the 8 KiB limit.
- A query returns enum-only `NONE`, `CANDIDATE` or `BLOCKED` information. A
  candidate always requires human approval and has no execution authority.
- V0.1 has no persistence. Restarting or `RESET` clears its learned structure.
- `l2_fixture_cli` is an explicitly invoked offline adapter for the frozen
  synthetic CSV format. It is not a live observer or background service.

Different A/B results prove only deterministic dependence on different
synthetic histories. They do not establish useful personalization,
individuality, real-workflow transfer, or user benefit.

## API

- `l2_init(state)` creates a canonical empty state.
- `l2_step(state, event, result)` validates and applies one event. Stable graph
  counters change only when a valid session of at least three roles ends.
- `l2_query_workspace(state, workspace, now_ms)` is read-only and returns a
  bounded result using current support and context.

The fixed capacities are four workspaces, 16 nodes, 32 directed edges, 16
ordered triples and eight pending roles. Counts saturate; deterministic aging
occurs every 16 eligible completed sessions.

## Reproduction in Debian

```sh
cd /home/lab/hmi/src/level2
make corpus-verify
make ci
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 make sanitize
make bench
make qualification
```

Example finite replay:

```sh
./l2_fixture_cli evaluation/fixtures_v1/a.csv
```

Only the checked-in simulated fixtures are authorized for this milestone.

## V0.2a offline evaluator preflight

`evaluation/v02a/` is a finite, standard-library-only test harness for the
scientific measurement path. It keeps permitted pre-query histories, permitted
query safety context and hidden future-task truth in separate files; executes
the frozen V0.1 observer plus recent/static/NONE comparators; checks a toy
known-answer scorer; and verifies a canonical SHA-256 calibration lock before
opening one explicitly named non-study sentinel.

```sh
make v02a-fixtures
make v02a-preflight
make v02a-bench
```

These targets do not generate or score a V0.2 held-out study. A passing
preflight establishes evaluator-tooling readiness only. It does not establish
L2 utility, appropriate individuality, real-workflow transfer, monitoring or
OS-action authority.
