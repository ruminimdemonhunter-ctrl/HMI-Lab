#!/bin/sh
set -eu

historical="evaluation/r23b_corpus_v2/machine_a_train.csv"
expected="e56270cd42b3c923d5e666c79318949357f877a107c03e2a36286c9c430cae52"
actual="$(sha256sum "$historical" | awk '{print $1}')"
test "$actual" = "$expected"

./l1_r23c_support_preflight "$historical"
./l1_r23c_decision_preflight
python3 evaluation/verify_r23c_lock.py
python3 evaluation/verify_r23c_probes.py
python3 evaluation/verify_r23c_eligibility.py

echo "R2_3C_PREFLIGHT=PASS gates=A|B|C|D|E heldout_access=NONE"
echo "R2_UTILITY=UNDETERMINED R2_INDIVIDUALITY=UNDETERMINED"
