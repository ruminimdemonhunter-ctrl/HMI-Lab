#!/bin/sh
set -eu

expect_reject() {
    if ./l1_recorder_bench "$@" >/dev/null 2>&1; then
        echo "FAIL: recorder benchmark accepted invalid arguments: $*" >&2
        exit 1
    fi
}

expect_reject
expect_reject unknown 5
expect_reject baseline 1
expect_reject candidate 86401
expect_reject baseline +5
expect_reject candidate 5 extra

if ./l1_psi_bench unknown 5 >/dev/null 2>&1 ||
   ./l1_psi_bench baseline 0 >/dev/null 2>&1 ||
   ./l1_psi_bench candidate 86401 >/dev/null 2>&1; then
    echo "FAIL: PSI source benchmark accepted invalid arguments" >&2
    exit 1
fi

echo "PASS: recorder benchmark arguments and bounds"
