#!/bin/sh
set -eu

manifest="${TMPDIR:-/tmp}/l1-v06-args-$$.eval.manifest"
wrong="${manifest}.wrong"
trap 'rm -f "$manifest" "$wrong"' EXIT HUP INT TERM

expect_failure() {
    if ./l1_evaluate_v06 "$@" >/dev/null 2>&1; then
        echo "FAIL: V0.6 evaluator accepted invalid input: $*" >&2
        exit 1
    fi
}

expect_failure
expect_failure train
expect_failure heldout "$manifest" 0
expect_failure heldout "$manifest" 10001
expect_failure heldout "$manifest" 1x

development="$(./l1_evaluate_v06 development)"
case "$development" in
    "DEVELOPMENT_V06,scenarios=24,checksum=adafe2486842d831,invalid=200,regressions=13,resets=4,corrupt=2") ;;
    *) echo "FAIL: V0.6 development generator drift: $development" >&2; exit 1 ;;
esac

training="$(./l1_evaluate_v06 train "$manifest")"
case "$training" in
    TRAIN_V06,action_candidates=108,forecast_candidates=64,development_checksum=adafe2486842d831,training_checksum=69e07b6e49c323f6,action_loss=14142.00,policy=2000\|50\|300\|2,forecast_errors=333,forecast_windows=2084,forecast=2000\|75\|600,fingerprint=12bbce7fc6b50db7,manifest=*) ;;
    *) echo "FAIL: V0.6 training selection drift: $training" >&2; exit 1 ;;
esac

sed 's/training_checksum=69e07b6e49c323f6/training_checksum=69e07b6e49c323f7/' \
    "$manifest" > "$wrong"
expect_failure heldout "$wrong" 1

echo "PASS: V0.6 arguments, split checksums, training identity, and manifest rejection"
