#!/bin/sh
set -eu

expect_reject() {
    if ./l1_trace_record "$@" >/dev/null 2>&1; then
        echo "FAIL: recorder accepted invalid arguments: $*" >&2
        exit 1
    fi
}

expect_reject
expect_reject --output relative.l1trace --samples 5
expect_reject --output /tmp/x.l1trace
expect_reject --output /tmp/x.l1trace --samples 1
expect_reject --output /tmp/x.l1trace --samples 5 --duration-sec 5
expect_reject --output /tmp/x.l1trace --samples 5 --cadence-ms 999
expect_reject --output /home/lab/hmi/src/x.l1trace --samples 5

echo "PASS: recorder requires safe absolute output and finite bounded capture"
