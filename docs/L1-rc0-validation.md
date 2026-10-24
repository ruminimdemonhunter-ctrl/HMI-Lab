# HMI Level 1 RC0 qualification report

## Verdict

    RC0_ENGINEERING: GO
    RC0_HUMAN_PACKET: READY
    V1_2_BENEFIT_EXPERIMENT: BLOCKED
    REAL_RESOURCE_BENEFIT: NOT_ESTABLISHED

`READY` means the documentation packet is complete. Its visible
`NOT YET RELEASED FOR PERSONAL ACCEPTANCE` banner remains in force pending
merge and independent review. RC0 does not authorize personal assessment,
actuation, L2/L3 work or a benefit experiment.

Protocol: `5c4df3d592fa69115f358530681e47112fb3f0cd` on base
`1401f51cf094da72a4981efb0ddc70f656bf20c1`. Qualification was performed on
the exact working tree represented by this report before its delivery commit;
the final delivery SHA and CI run links are recorded in the PR.

## Environment and commands

- Debian QEMU guest: Linux `6.12.113+deb13-amd64`, x86_64.
- Compiler: Debian GCC `14.2.0-19`.
- Strict flags: `-std=c11 -O2 -Wall -Wextra -Werror -Wpedantic`.
- Sanitizers: `-O1 -g -fsanitize=address,undefined
  -fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=1`, and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

The guest ran sequentially:

    make rc0-qualification

This performed clean strict normal and NDEBUG builds/tests, ASan, UBSan and
leak checks, 100 seeded replays, all frozen V0.4--V0.7 evaluations,
recorder/trace/parser/state/policy/proposal tests, optimized full component
benchmarks and the protected-source audit. It finished with:

    RC0_QUALIFICATION_COMMANDS=PASS
    V1_2_BENEFIT_EXPERIMENT=BLOCKED

The bounded CI composition also passed:

    make rc0-ci
    RC0_CI=PASS

No suite was skipped, no failing repetition was discarded and sanitizer RSS
was not substituted for optimized RSS. The initial source synchronization was
attempted while the existing VM was stopped and received connection refused;
the existing guest was then started, non-interactive SSH verified, and all
qualification commands completed. This was an environment startup issue, not
a test failure or host configuration change.

## RC0 additions and cross-component results

`l1_rc0_test` uses the compiled core, shadow, state and test-only proposal
modules rather than duplicate decision logic. Normal, NDEBUG and sanitizer
runs passed these checks:

- `sizeof(L1Core) == 104`, `sizeof(L1Shadow) == 208`, V2 state = 72 bytes;
- memory protection > I/O deferral > interactive CPU > HOLD;
- invalid and regressing timestamps publish HOLD;
- inserting shadow observation never changes action or any core byte compared
  with the observer-off reference;
- invalid policy, denied proposal, stale proposal and base mismatch preserve
  every current/result core byte;
- a positive test-only exact-byte verifier applies only to an isolated clone,
  resets to HOLD, and supports monotonic generation rollback;
- persisted V2 corruption recovers exactly to default/HOLD.

`rc0_source_audit.sh` verified frozen SHA-256 identities for the protected
core, shadow, policy, state and public headers, rejected forbidden resource-
control interfaces, and confirmed the production proposal checker does not
link the test-only apply seam. No core bug was found. No protected algorithm,
compiled bound, priority, learner or state format changed.

## Full optimized resource qualification

All values are five repetitions in the isolated guest. Timing variation is
reported rather than hidden and CPU time remains only an overhead proxy.

| Component | Full input and CPU result | Peak RSS | Frozen gate |
|---|---|---:|---|
| Inspector | 86,400 rows; CLI CPU 2,332.917 ns/row median (2,220.498..2,430.706); wall 2,337.329 (2,223.968..2,439.910) | 1,660 KiB median (1,600..1,712) | <=8,192 KiB: PASS |
| Evidence | 86,400 rows; CPU 1,306.898 ns/row median (1,253.981..1,535.359); wall 1,309.615 (1,256.623..1,538.789) | 1,484 KiB median (1,408..1,584) | <=16,384 KiB: PASS |
| Proposal | canonical plus maximum negative pair; CPU 3,367,000 ns median (3,001,000..7,786,000); wall 3,647,100 (3,252,600..8,159,100) | 1,416 KiB median (1,348..1,472) | <=8,192 KiB: PASS |

Inspector phase medians were parser 552.485, core+shadow 67.821, formatting
544.711 and complete replay 619.707 CPU ns/row. The inspector trace checksum
was `44084e4b9d6b7a83`; evidence was `914b847cf8020b12`.

Recorder paired synthetic measurements remained value-equivalent. At 180
samples, baseline total CPU was 64,833.333 ns/sample median
(59,400.000..90,077.778), candidate 14,772.222
(14,133.333..20,405.556); candidate RSS was 1,404 KiB median, max 1,556 KiB.
The 5/65/180 checksums remained exact. Live PSI source profiling was read-only
and finite; it generated no workload and was not a trace capture.

Standalone five-run timing:

| Scope | Median or observed range |
|---|---|
| fixed core | 1.884 ns/sample median (1.755..2.267) |
| L1 core | 12.729 ns/sample median (11.714..13.223) |
| fixed observation-to-decision | 77,284.430 ns/sample median (76,186.950..83,448.190), RSS max 1,700 KiB |
| L1 observation-to-decision | 80,330.060 ns/sample median (77,557.390..86,853.740), RSS max 1,700 KiB |
| core in shadow benchmark | 17.260 ns/sample median (17.171..18.406) |
| shadow core | 53.060 ns/sample median (51.214..55.395) |
| observation plus L1 | 76,534.700 ns/sample median (75,096.800..83,629.060) |
| observation plus shadow | 77,086.800 ns/sample median (74,103.790..79,751.280), RSS max 1,760 KiB |

Microbenchmarks are VM-, compiler-, cache- and scheduling-sensitive. They do
not measure energy or causal savings and are not evidence of intelligence.

## Reproducibility identities

| Corpus | Verified identity |
|---|---|
| V0.4 development / training / held-out | `a2336c7fdbef3be7` / `dd1452338fbb778b` / `2cf70aab5f52315f` |
| V0.5 development / training / held-out | `35fc183a34be2268` / `981d7ab766c8556c` / `64568259bcad8d75` |
| V0.6 development / training / held-out | `adafe2486842d831` / `69e07b6e49c323f6` / `1f369de28902db46` |
| V0.7 development stream / schedule | `db58eecd0c03ae79` / `7bac0d8fe83eced2` |
| V0.7 verification stream / schedule | `89a76049e04fc3e7` / `07867dce0615e19c` |
| V0.7 held-out stream / schedule | `aed51578256958b0` / `85c31afc0b01be04` |
| V0.8 recorder 5 / 65 / 180 | `629043ecc32a1a35` / `4bedf64151165f55` / `d1762306024e69f6` |
| V1.0 clean / saturation / 86,400-row | `f2aca8af7c6ddd3c` / `0bbd76851e142c45` / `914b847cf8020b12` |
| V1.1 canonical proposal SHA-256 | `fc689e52133e74c68419941351aeb58f74ab8e8dd7d64526a224fc33e1ff7f04` |

The ten V0.9 summary checksums remained, in declared order:
`54af6364d075f966`, `952ad459e7ff30b3`, `795181e9309912d5`,
`1ede234d92dde15e`, `886abf8fd06a1502`, `4b81ae005eb42cba`,
`9d405c791f2efb0d`, `9b661be5514c4c15`, `4cca8b96837fbd19`,
`cafd0541f68605d8`. Across 82 rows, observer mismatches were zero.

## Historical verdicts retained

| Work | Unchanged verdict |
|---|---|
| V0.4/V0.5 bounded adaptation | `NO_GO` |
| V0.6 shadow / coupling | `SHADOW_PREDICTION: NO_GO`; `ACTION_COUPLING: NO_GO` |
| V0.7 diagnostics | `DIAGNOSTICS: NO_GO` |
| V0.8 recorder / evidence | `RECORDER_EFFICIENCY: GO`; `REAL_EVIDENCE: INSUFFICIENT` |
| V0.9 replay | `REPLAY_READY: GO` |
| V1.0 analyzer | `EVIDENCE_TOOLING: GO`; `REAL_EVIDENCE: INSUFFICIENT`; `FORECAST_TRANSFER: UNDETERMINED` |
| V1.1 proposal boundary | `PROPOSAL_VALIDATION: GO`; `PRODUCTION_AUTHORIZATION: BLOCKED`; `L2_DEPLOYMENT: NOT_AUTHORIZED` |

## Evidence limitations and source audit

The full-size inputs are deterministic synthetic traces, not 86,400 seconds
of a live workload. No research-length PSI capture was started, no stress was
generated and no raw trace was uploaded. Tests establish deterministic
engineering behavior, bounded memory use and safety fallback, not general
learning, production authentication, workload optimization, resource savings
or energy benefit.

The Git diff and tracked-file inventory were checked for binaries, credentials,
VM disks, backups and raw recordings. Executables remain ignored and outside
the commit. The only proposal verifier capable of approval is compiled under
`L1_TESTING`; the production CLI remains deny-only. No scheduler, cgroup,
kernel, device, firmware, network-update, hardware-control, L2 or L3 interface
was introduced.

Independent PR review and passing remote CI remain delivery gates. The PR must
stay open and unmerged until that review is complete.
