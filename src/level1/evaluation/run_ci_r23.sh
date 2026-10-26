#!/bin/sh
set -eu

output="$(mktemp)"
trap 'rm -f "$output"' EXIT HUP INT TERM

python3 evaluation/verify_r23_lock.py \
    --lock evaluation/r23_calibration_lock_v1.txt \
    --source-dir . \
    --corpus evaluation/r22_corpus_v1 \
    --evaluator ./l1_evaluate_r23

./l1_evaluate_r23 heldout evaluation/r22_corpus_v1 \
    evaluation/r23_calibration_lock_v1.txt >"$output"

actual="$(sha256sum "$output" | awk '{print $1}')"
expected="1a2b082d3354bdcc5ca7c19ca2a12d120139d41877dac7c6b27c9fa223a8b087"
test "$actual" = "$expected"
grep -F 'R23_HELDOUT_COMPLETE no_retuning=YES R2_3_STUDY=INVALID R2_UTILITY=UNDETERMINED R2_INDIVIDUALITY=UNDETERMINED' "$output" >/dev/null
grep -F 'MACHINE_GATE machine=A support=PASS strongest=fixed-tuned noninferior=FAIL family_win=FAIL' "$output" >/dev/null
grep -F 'MACHINE_GATE machine=B support=PASS strongest=fixed-tuned noninferior=FAIL family_win=FAIL' "$output" >/dev/null

echo "R23_HELDOUT_REPLAY status=PASS sha256=$actual"
