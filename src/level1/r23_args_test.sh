#!/bin/sh
set -eu

expect_failure() {
    if "$@" >/dev/null 2>&1; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

expect_failure ./l1_evaluate_r23
expect_failure ./l1_evaluate_r23 unknown evaluation/r22_corpus_v1
expect_failure ./l1_evaluate_r23 calibrate
expect_failure ./l1_evaluate_r23 calibrate /definitely/not/a/corpus
expect_failure ./l1_evaluate_r23 heldout evaluation/r22_corpus_v1
expect_failure ./l1_evaluate_r23 heldout evaluation/r22_corpus_v1 /definitely/not/a/lock

echo "PASS: R2.3 evaluator mode, arity, and pre-lock failure bounds"
