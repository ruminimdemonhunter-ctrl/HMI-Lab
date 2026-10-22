#!/bin/sh
set -eu

manifest="${TMPDIR:-/tmp}/l1-v05-args-$$.eval.manifest"
wrong="${manifest}.wrong"
trap 'rm -f "$manifest" "$wrong"' EXIT HUP INT TERM

expect_failure() {
    if ./l1_evaluate_v05 "$@" >/dev/null 2>&1; then
        echo "FAIL: V0.5 evaluator accepted invalid input: $*" >&2
        exit 1
    fi
}

expect_failure
expect_failure heldout "$manifest" 0
expect_failure heldout "$manifest" 10001
expect_failure heldout "$manifest" 1x
expect_failure train

development="$(./l1_evaluate_v05 development)"
case "$development" in
    "DEVELOPMENT_V05,scenarios=24,checksum=35fc183a34be2268,invalid=186,regressions=11") ;;
    *) echo "FAIL: V0.5 development generator drift: $development" >&2; exit 1 ;;
esac

training="$(./l1_evaluate_v05 train "$manifest")"
case "$training" in
    TRAIN_V05,candidates=108,checksum=981d7ab766c8556c,loss=7290.00,cpu=3000,memory=100,io=500,confirmation=2,fingerprint=5147f61840da7089,manifest=*) ;;
    *) echo "FAIL: V0.5 training drift: $training" >&2; exit 1 ;;
esac

sed 's/training_checksum=981d7ab766c8556c/training_checksum=981d7ab766c8556d/' \
    "$manifest" > "$wrong"
expect_failure heldout "$wrong" 1

echo "PASS: V0.5 arguments, checksums, training identity, and manifest rejection"
