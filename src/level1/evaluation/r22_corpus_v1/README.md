# R2.2 frozen synthetic corpus V1

This directory contains deterministic, nonpersonal synthetic PSI-style
observations. It contains no real host or guest telemetry. Labels and episode
boundaries are engineered research fixtures, not natural-world truth.

Each machine has 40 ordered 320-row sessions: sessions 0--23 are training,
24--31 calibration and 32--39 untouched held-out. Row IDs include machine,
split, session and row and never overlap. Each session declares four normal
and three positive independent scoring windows in a separate episode CSV.
Held-out therefore contains 32 normal and 24 positive windows per machine,
before any R2.3 scoring exclusions, satisfying the preregistered minimums.

The generator uses independent deterministic truth and observation-noise LCG
streams. Seeds are the exact R2.0/R2.2 values. `shared_probe`, `regime_swap`
and `faults` are disjoint from all split row IDs. Fault rows exercise absent,
invalid, stale, regressed, reset, unknown-quality and out-of-range inputs.

`r23_fixed_comparator_v1.json` is frozen now but is not scored in R2.2.
Held-out labels must not be used to revise memory or comparator behavior.

Rebuild and verify every byte with:

    python3 evaluation/verify_r22_corpus.py \
      --corpus evaluation/r22_corpus_v1

`SHA256SUMS` covers all data, episode, comparator, README and JSON manifest
files. Its own SHA-256 is the aggregate corpus identity printed by the
verification utility.
