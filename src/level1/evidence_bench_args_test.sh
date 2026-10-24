#!/bin/sh
set -eu

expect_fail() {
    if "$@" >/dev/null 2>/dev/null; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

expect_fail ./l1_evidence_bench relative 1 65
expect_fail ./l1_evidence_bench /tmp/missing 0 65
expect_fail ./l1_evidence_bench /tmp/missing 6 65
expect_fail ./l1_evidence_bench /tmp/missing 999999999999999999999 65
expect_fail ./l1_evidence_bench /tmp/missing 1 1
expect_fail ./l1_evidence_bench /tmp/missing 1 86401
expect_fail ./l1_evidence_bench /tmp/missing 1 not-a-number
expect_fail ./l1_evidence_bench /tmp/missing 1 65 unknown-mode
expect_fail sh ./run_evidence_benchmark.sh 1 1
expect_fail sh ./run_evidence_benchmark.sh 65 0
expect_fail sh ./run_evidence_benchmark.sh 65 6
expect_fail sh ./run_evidence_benchmark.sh 65 1 unknown-mode

echo "PASS: evidence benchmark argument grammar and limits"
