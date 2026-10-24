# HMI L1 RC0 — frozen offline stability / release qualification plan

**Status: preregistered initial branch commit; frozen before RC0 implementation and before any RC0 result evaluation.**

Base: V1.1 merged `main` at `1401f51cf094da72a4981efb0ddc70f656bf20c1`.
Work item: [Issue #19](https://github.com/ruminimdemonhunter-ctrl/HMI-Lab/issues/19).
Original multi-milestone acceptance: [Issue #13](https://github.com/ruminimdemonhunter-ctrl/HMI-Lab/issues/13).

## Exact goal and non-goals

Deliver a reproducible, low-overhead **read-only engineering qualification** of the existing L1 and a corrected, honest draft personal-inspection packet. Qualification verifies software safety and stability under bounded adversarial and deterministic synthetic inputs. It cannot establish model intelligence, causal resource reduction, production authentication, or permission to actuate anything.

The separate **V1.2 benefit experiment remains BLOCKED** until independently sufficient scientific evidence and explicit guest-only actuation authorization. Long research PSI recording remains separately opt-in with a user-selected finite duration and guest-local output path; neither is authorized by this plan, issue, branch or a generic instruction to continue.

Preserve the original HMI L1→L2→L3 hierarchy: immutable compiled safety authority at L1; future L2 policy proposals only within a bounded memory contract; never L3→L1 direct. L1 core is 104B, shadow 208B, stored V2 state exactly 72B, priority memory > I/O > CPU > HOLD; invalid/time-regressing input returns HOLD; no enabled actuator.

## Pre-frozen historical verdicts

| Work | Existing verdict |
|---|---|
| V0.4/V0.5 bounded adaptive behavior | NO_GO (previous frozen evaluations) |
| V0.6 shadow forecaster | SHADOW_PREDICTION NO_GO; ACTION_COUPLING NO_GO |
| V0.7 diagnostics | NO_GO (historical recorder CPU gate) |
| V0.8 read-only PSI recorder optimization | RECORDER_EFFICIENCY GO under synthetic benchmark |
| V0.9 transparent replay | REPLAY_READY GO |
| V1.0 offline evidence analyzer | EVIDENCE_TOOLING GO; REAL_EVIDENCE INSUFFICIENT; FORECAST_TRANSFER UNDETERMINED |
| V1.1 test-only proposal boundary | PROPOSAL_VALIDATION GO; PRODUCTION_AUTHORIZATION BLOCKED; L2_DEPLOYMENT NOT_AUTHORIZED |
| Real device/OS resource savings | NOT_ESTABLISHED |

These findings are preserved unchanged even when an environment-dependent RSS or wall-clock rerun differs. An RC0 GO is not permission to change a historical scientific result.

## Freeze scope and identity

Do not modify `l1_core.c`, `l1_shadow.c`, `l1_policy.c`, `l1_state.c`, persisted state formats, compiled behavior limits, published action ordering/fallback, previous evaluation inputs/manifests/expected outputs or long PSI capture policy. If a genuine bug requires core change, STOP and create a separately reviewed corrective protocol; RC0 must not silently widen scope.

Implement a **small orchestration and cross-component regression layer** around existing executables. Prefer POSIX shell + Make targets for Linux QEMU, optionally a tiny Python stdlib-only report utility already depended upon by tests. No new production runtime, service, package dependency, network operations, dynamic core allocation or Windows-specific privileged scripts.

## Fixed scenario matrix

Existing 10 inspector demo cases form the visible explanation dataset:
`idle-noise`, `rising-cpu`, `urgent-memory`, `sustained-io`, `overlap-priority`, `invalid-telemetry`, `monotonic-regression`, `reset-recovery`, `policy-rollback`, `shadow-prediction`.

Gate invariants on actual compiled core and independently expected results:
- protect-memory outranks defer-I/O outranks interactive-CPU outranks HOLD under overlapping valid pressures; confirmation streak requirements still apply.
- invalid telemetry and time regression immediately return HOLD, reset unsafe partial confirmation and never publish a shadow forecast as a decision.
- policy bounds, stale generation and rejection leave original core unchanged; only **test-only cloned** proposal approval may demonstrate fresh-generation HOLD reset and monotonic rollback.
- corrupt/truncated/version-mismatched V1/V2 persistence safely recovers to defaults/HOLD; no silent reinterpretation of migrated state; V2 encoding remains 72 bytes.
- strict malformed/truncated/oversized V1 traces reject before publishing partial user-visible details. Read-only replay action and `L1Core` bytes match independent observer-off reference on every tested row.
- canonical V1.1 envelope parser refuses unknown/duplicate/CRLF/overflow/stale/symlink-parent/mutable-path bytes; offline CLI never returns authentication/production authorization and mock positive verifier exists only under `L1_TESTING`.
- no non-test linkage from `l1_evidence`, `l1_inspect`, `l1_proposal_check` to an actuator or L2/L3; audit source and CLI options.

For reproducibility, immutable identity corpus includes existing V0.4/V0.5/V0.6/V0.7 held-out and verification seeds/hashes, V0.8 recorder mock trace identities, V0.9 inspector goldens, V1.0 clean/saturation V1 trace checksums `f2aca8af7c6ddd3c` and `0bbd76851e142c45`, V1.1 canonical proposal example SHA-256 `fc689e52133e74c68419941351aeb58f74ab8e8dd7d64526a224fc33e1ff7f04`. Do not replace historical fixtures.

## Frozen qualification matrix

Two runner entrypoints:

**Fast `rc0-ci`**: strict normal test compilation and synthetic scenario/fault/invariant smoke, existing historical CI identity checks, bounded recorder/inspect/evidence/proposal smoke, side-effect checks. Finish promptly on GitHub Actions; do not require Debian guest, actual PSI capture, host SSH, actual 86,400 seconds or privileged permissions.

**Full `rc0-qualification`** in Debian QEMU on explicit invocation: clean strict normal C11 build + all tests, independent `NDEBUG`, ASan/UBSan/leak, seeded replay, previous frozen held-out evaluations, parser and migration, V0.8 recorder, V0.9 inspect, V1.0 evidence and V1.1 proposals, then rebuild optimized (not sanitizer) and run full synthetic **86,400-row** maximum for inspector and evidence with their existing five repetitions and optimized recorder/proposal benchmark phases where defined. Never confuse virtual rows with actual live recording wall time. Stop on any failure. Avoid destructive persistent-state tests outside temporary test dirs.

**Budgets remain component-specific**, no fabricated aggregate pass:
- V0.9 inspector optimized peak RSS <=8,192 KiB;
- V1.0 evidence optimized peak RSS <=16,384 KiB;
- V1.1 proposal optimized peak RSS <=8,192 KiB;
- V0.8 recorder frozen individual efficiency/per-sample + RSS criterion remains as published in its plan;
- core/shadow standalone performance tests retain their historical limits/GO/NO-GO, with full per-component CPU/wall/RSS and five run min/median/max reported separately. No sanitizer RSS used to replace an optimized budget; sanitizer still must not report memory errors/leaks.
- Do not invent a total-device energy or host net-savings measurement: `REAL_RESOURCE_BENEFIT: NOT_ESTABLISHED`.

All failure conditions are blockers for `RC0_ENGINEERING:GO`. Do not silently exclude outlier runs, shorten frozen historical evaluations, alter false-positive/false-negative scoring, skip a flaky failure, or treat a missing tool as PASS. If a command is unexecutable in the tested environment, state `NOT_RUN` and `RC0_ENGINEERING:NO_GO` until resolved or protocol-amended *before* inspecting the outcome.

## Report format and provenance

Produce `docs/L1-rc0-validation.md` with: exact git SHA, Debian guest kernel/compiler/architecture, explicit compiler flags; command matrix with PASS/FAIL/NOT_RUN and complete iteration count; per-component benchmark CPU/wall/RSS and frozen budget; exact frozen fixture identities; distinct normal/NDEBUG/ASan/UBSan outcomes; 104B/208B/72B identity; changes/new tests; command output summary and known limits.

A bounded machine-readable summary (e.g. key=value plain text with controlled characters) may aid audit, but MUST NOT include a raw personal trace, guest private path, username, machine ID, credential, host-specific software inventory or copied private data. A self-authored test report is not an independent attestation. Human review of PR remains required.

**Four independent statuses in report**:
- `RC0_ENGINEERING: GO|NO_GO` (integrated deterministic safety, tests, bounded resources);
- `RC0_HUMAN_PACKET: READY|NOT_READY` (documentation completeness only; *does not start the personal review*);
- `V1_2_BENEFIT_EXPERIMENT: BLOCKED`;
- all historic scientific/security statuses exactly retained.

Use optimized synthetic trace row caps, buffered output limits, safe temp files with cleanup; no hanging daemons/looping watchers. Do not create sessions or automatically schedule follow-ups.

## Personal acceptance packet preparation (not activation)

Update `docs/L1-human-acceptance.md` after tests to refer to a qualified **RC0 SHA**, NOT stale V0.9 branch. Include:
- host Windows PowerShell `cd C:\Dev\Projects\HMI-Lab`, `git switch main` and `git pull --ff-only origin main` after release integration; optional explicit-branch instructions for review without claiming final merge;
- only isolated Debian QEMU with `scripts\sync-source.ps1` and `ssh -p 2222 lab@127.0.0.1`, plus `make clean all test`, `./l1_inspect demo --details 0` and named read-only demos;
- expected label semantics: published advisory action vs shadow-only forecast vs fixed reference; rejection/fallback/rollback, invalid inputs, known weaknesses/negative historical GO criteria; only run examples that exist in repository;
- Ctrl+C finite-process stop, backup/guest-state cautions, no automatic OS resource adjustments, and a human scorecard with free-form proposed behavior changes;
- visible banner **NOT YET RELEASED FOR PERSONAL ACCEPTANCE** until engineering verification and scientific reviewer approve an RC0 commit. It is correct to have a complete packet before requesting any user judgment.

## Delivery and stop rule

Commit this frozen plan as the first RC0 branch change. Implement tests/docs/report/orchestration without touching the core, maintain Linux CI and Debian QEMU proof, open an **unmerged** PR against main. Do not infer authorization to merge. Any future research-length read-only recording must be separately authorized with **exact finite duration and guest-local destination**. Any guest-only actuation is a separate gated milestone. No host Windows change, privileged OS/scheduler/cgroup writes, PSI trigger registration, stress workload or networking/upload.

The first human assessment is requested only after an engineering-qualified and independently reviewed stable L1 candidate exists. Even then user acceptance may reveal desired changes; it must not be presented as proven learning or resource savings.
