#!/bin/sh
set -eu

expect_fail() {
    if "$@" >/dev/null 2>&1; then
        echo "FAIL: command unexpectedly succeeded: $*" >&2
        exit 1
    fi
}

expect_fail ./l1_evaluate_r23b
expect_fail ./l1_evaluate_r23b calibrate
expect_fail ./l1_evaluate_r23b heldout evaluation/r23b_corpus_v2
expect_fail ./l1_evaluate_r23b probes evaluation/r23b_corpus_v2
expect_fail ./l1_evaluate_r23b unknown evaluation/r23b_corpus_v2

echo "PASS: R2.3b evaluator mode, arity, and pre-lock held-out bounds"
