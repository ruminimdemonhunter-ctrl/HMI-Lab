#!/bin/sh
set -eu

expect_reject() {
    if ./l1_diagnostics "$@" >/dev/null 2>&1; then
        echo "FAIL: diagnostics accepted invalid arguments: $*" >&2
        exit 1
    fi
}

expect_reject
expect_reject heldout 0
expect_reject heldout 101
expect_reject trace missing.l1trace live-default
expect_reject trace missing.l1trace v06-trained
expect_reject trace missing.l1trace unknown
expect_reject unknown

echo "PASS: diagnostics mode, repetition, trace, and configuration arguments"
