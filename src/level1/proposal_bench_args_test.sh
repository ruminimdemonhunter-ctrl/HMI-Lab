#!/bin/sh
set -eu

expect_fail() {
    if "$@" >/dev/null 2>/dev/null; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

expect_fail ./l1_proposal_bench relative /tmp/negative 1
expect_fail ./l1_proposal_bench /tmp/valid relative 1
expect_fail ./l1_proposal_bench /tmp/valid /tmp/negative 0
expect_fail ./l1_proposal_bench /tmp/valid /tmp/negative 6
expect_fail ./l1_proposal_bench /tmp/valid /tmp/negative 999999999999
expect_fail ./l1_proposal_bench /tmp/valid /tmp/negative 1 unknown
expect_fail sh ./run_proposal_benchmark.sh 0 optimized
expect_fail sh ./run_proposal_benchmark.sh 6 optimized
expect_fail sh ./run_proposal_benchmark.sh 1 unknown

echo "PASS: proposal benchmark arguments and bounds"
