#!/bin/sh
set -eu

policy="${TMPDIR:-/tmp}/l1-v04-args-$$.eval.policy"
wrong_policy="${policy}.wrong"
trap 'rm -f "$policy" "$wrong_policy"' EXIT HUP INT TERM

expect_failure() {
    if ./l1_evaluate "$@" >/dev/null 2>&1; then
        echo "FAIL: evaluator accepted invalid arguments: $*" >&2
        exit 1
    fi
}

expect_failure
expect_failure heldout "$policy" 0
expect_failure heldout "$policy" 10001
expect_failure heldout "$policy" 1x
expect_failure train

development="$(./l1_evaluate development)"
case "$development" in
    "DEVELOPMENT,scenarios=20,checksum=a2336c7fdbef3be7,invalid=186,regressions=17") ;;
    *) echo "FAIL: development generator drift: $development" >&2; exit 1 ;;
esac

training="$(./l1_evaluate train "$policy")"
case "$training" in
    TRAIN_SELECTED,candidates=108,seeds=101\|211\|307\|401,checksum=dd1452338fbb778b,loss=5113.00,cpu=3000,memory=100,io=500,confirmation=2,policy_fingerprint=5147f61840da7089,output=*) ;;
    *) echo "FAIL: training selection drift: $training" >&2; exit 1 ;;
esac

sed 's/confirmation_samples=2/confirmation_samples=3/' "$policy" > "$wrong_policy"
expect_failure heldout "$wrong_policy" 1

echo "PASS: evaluator arguments, checksums, training selection, and frozen policy identity"
