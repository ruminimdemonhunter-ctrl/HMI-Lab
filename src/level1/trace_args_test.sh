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

base="/tmp/hmi-l1-recorder-test-$$"
race="$base-race.l1trace"
duration="$base-duration.l1trace"
samples="$base-samples.l1trace"
cleanup() {
    rm -f "$race" "$race.partial" "$duration" "$duration.partial" \
        "$samples" "$samples.partial"
}
trap cleanup EXIT HUP INT TERM

./l1_trace_record --output "$race" --samples 3 --cadence-ms 1000 \
    >/dev/null 2>&1 &
recorder_pid=$!
attempt=0
while [ ! -e "$race.partial" ] && kill -0 "$recorder_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 200 ]; then
        echo "FAIL: recorder temporary file did not appear" >&2
        exit 1
    fi
    sleep 0.01
done
printf 'review-sentinel\n' >"$race"
sentinel_before=$(cksum <"$race")
if wait "$recorder_pid"; then
    echo "FAIL: recorder replaced a destination created during capture" >&2
    exit 1
fi
sentinel_after=$(cksum <"$race")
if [ "$sentinel_before" != "$sentinel_after" ] || [ -e "$race.partial" ]; then
    echo "FAIL: publication race changed destination or retained temporary file" >&2
    exit 1
fi

./l1_trace_record --output "$duration" --duration-sec 2 --cadence-ms 1000 \
    >/dev/null
./l1_diagnostics trace "$duration" live-default >/dev/null
./l1_trace_record --output "$samples" --samples 3 --cadence-ms 1000 \
    >/dev/null
./l1_diagnostics trace "$samples" live-default >/dev/null

echo "PASS: recorder bounds, atomic no-replace publication, and duration boundary"
