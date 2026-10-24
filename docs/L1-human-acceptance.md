# HMI Level 1 RC0 human acceptance guide

> **RELEASED FOR PERSONAL READ-ONLY ACCEPTANCE — RC0**
>
> The RC0 implementation at `56303df90c9e412df2279d2ccb5fa8e31d17ed88`
> was independently reviewed and integrated via PR #20, merge commit
> `3a90220c09afedc1b222f89b319910a37943fe28`.
> The owner has authorized opening the personal evaluation of *advisory*
> behavior and explainability. This does not authorize production use,
> real PSI research recording, workload generation, L2/L3 deployment, guest
> resource actuation, or Windows host/hardware changes. Stop if any
> engineering check fails.

## Scope and safety

RC0 is a read-only engineering candidate. L1 publishes advisory actions; it
does not apply them. The tools do not write policies, traces, cgroups,
scheduler settings, devices, firmware or hardware and do not generate
pressure. Shadow forecasts never alter the published action.

Passing this packet does not demonstrate adaptive learning, production policy
authorization or real resource savings. The frozen findings remain:

- V0.4/V0.5 adaptation: `NO_GO`.
- V0.6 shadow prediction: `NO_GO`; action coupling: `NO_GO`.
- V0.7 diagnostics: `NO_GO`.
- V0.8 recorder efficiency: `GO`; real evidence: `INSUFFICIENT`.
- V0.9 replay readiness: `GO`.
- V1.0 evidence tooling: `GO`; real evidence: `INSUFFICIENT`; forecast
  transfer: `UNDETERMINED`.
- V1.1 proposal validation: `GO` only for the test boundary; production
  authorization: `BLOCKED`; L2 deployment: `NOT_AUTHORIZED`.
- Real resource benefit: `NOT_ESTABLISHED`.

## Obtain the reviewed candidate

After a reviewed RC0 is integrated, use PowerShell from the Windows workspace:

    cd C:\Dev\Projects\HMI-Lab
    git switch main
    git pull --ff-only origin main

For archival inspection of the original reviewed PR branch (not the released
`main`), explicitly use:

    git switch feature/l1-rc0-offline-qualification

Confirm the existing isolated Debian QEMU guest is already running, then:

    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sync-source.ps1
    ssh -p 2222 lab@127.0.0.1

These commands synchronize source into the guest. Keep a recoverable guest
backup or snapshot according to the lab procedure before unrelated VM work;
never put disks or backups in Git.

## Reproduce engineering qualification

Inside the Debian guest:

    cd /home/lab/hmi/src/level1
    make rc0-ci
    make rc0-qualification

`rc0-ci` is finite and CI-sized. `rc0-qualification` is the sequential full
suite, including sanitizers, frozen historical evaluations and synthetic
86,400-row resource tests. It performs no long live capture. Expected terminal
markers are `RC0_CI=PASS`, `RC0_QUALIFICATION_COMMANDS=PASS` and
`V1_2_BENEFIT_EXPERIMENT=BLOCKED`.

Stop if a command reports failure. Do not reinterpret a partial run as a pass.

## Inspect actual L1 behavior

    ./l1_inspect demo --details 0
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

Interpret the output as follows:

- `published` is the advisory action returned by the real `l1_step_at()` core.
- `fixed` is a separate `l1_step_at()` reference with adaptation disabled.
- `candidate`, streaks and `reason` expose actual core state.
- `static_live` and `static_v06` are forecast references, not actions.
- `shadow_prediction` and feedback come from `l1_shadow_step_at()` and cannot
  change `published`.
- `observer_equivalence` must be `PASS`; mismatches must be zero.

Expected landmarks are delayed CPU confirmation, immediate eligible memory
protection, I/O deferral, memory-over-I/O-over-CPU priority, HOLD on invalid or
regressing observations, bounded proposal rejection/rollback and causally
delayed shadow feedback. A different result is a stop condition.

Persistence and authorization evidence can be reviewed separately:

    ./l1_state_test
    ./l1_proposal_test
    ./l1_rc0_test

The proposal verifier is test-only and is not production authentication. CRC
state integrity is not a trust anchor. Rejection must leave current core bytes
unchanged; a corrupt state must recover to defaults/HOLD; V2 remains 72 bytes.

## Optional private trace

Only use an already-recorded guest-local V1 trace that you explicitly choose.
Do not copy it into the synchronized source tree or upload it:

    ./l1_inspect trace /absolute/guest/path/example.l1trace --forecast-config live-default --details 20
    ./l1_inspect trace /absolute/guest/path/example.l1trace --forecast-config v06-trained --details 20

The checksum detects accidental corruption, not provenance or authentication.
Private traces lack independently established latent truth and cannot establish
causal benefit. A new research-length capture requires separate authorization
with an exact duration and guest-local output path.

## Personal scorecard

Record `clear`, `unclear` or `unacceptable`, with free-form notes:

| Question | Rating | Notes |
|---|---|---|
| Can you identify every published advisory action? | | |
| Are HOLD fallback and confirmation delay understandable? | | |
| Is memory > I/O > CPU priority visible? | | |
| Is shadow prediction unmistakably separate from action? | | |
| Is the fixed-controller comparison understandable? | | |
| Are rejection, corruption recovery and rollback understandable? | | |
| Are production-authorization limits explicit? | | |
| Are the negative scientific findings and missing benefit evidence explicit? | | |

For a proposed behavior change, record the scenario, row, observations,
current published action, desired advisory action and safety rationale. A note
does not authorize modifying priority, state formats, L2/L3 or actuation.

## Stop and recovery

All viewers and runners are finite foreground processes. Press Ctrl+C to stop;
they create no daemon or scheduled task. Benchmark temporary files are removed
on normal exit and handled signals. Preserve failure output and the exact Git
SHA. Do not proceed with personal assessment after a failed gate.
