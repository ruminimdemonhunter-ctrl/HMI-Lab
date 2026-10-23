#!/bin/sh
set -eu

expect_reject() {
    if ./l1_trace_record "$@" >/dev/null 2>&1; then
        echo "FAIL: recorder accepted invalid arguments: $*" >&2
        exit 1
    fi
}

expect_reject_message() {
    expected=$1
    shift
    message=$(timeout 5 ./l1_trace_record "$@" 2>&1) && {
        echo "FAIL: recorder accepted invalid arguments: $*" >&2
        exit 1
    }
    case "$message" in
        *"$expected"*) ;;
        *) echo "FAIL: recorder rejection lacked: $expected" >&2; exit 1 ;;
    esac
}

expect_reject
expect_reject --output relative.l1trace --samples 5
expect_reject --output /tmp/x.l1trace
expect_reject --output /tmp/x.l1trace --samples 1
expect_reject --output /tmp/x.l1trace --samples 5 --duration-sec 5
expect_reject --output /tmp/x.l1trace --samples 5 --cadence-ms 999
expect_reject --output /home/lab/hmi/src/x.l1trace --samples 5
bound="/tmp/hmi-l1-recorder-bound-$$.l1trace"
expect_reject_message "more than 86400 trace samples" \
    --output "$bound" \
    --duration-sec 86400 --cadence-ms 1000
if [ -e "$bound" ] || [ -e "$bound.partial" ]; then
    echo "FAIL: rejected duration/cadence pair created an output file" >&2
    exit 1
fi

base="/tmp/hmi-l1-recorder-test-$$"
race="$base-race.l1trace"
duration="$base-duration.l1trace"
samples="$base-samples.l1trace"
interrupted="$base-interrupted.l1trace"
profile_file="$base-profile.txt"
cleanup() {
    rm -f "$race" "$race.partial" "$duration" "$duration.partial" \
        "$samples" "$samples.partial" "$interrupted" "$interrupted.partial" \
        "$profile_file"
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

timeout 10 ./l1_trace_record --output "$duration" \
    --duration-sec 2 --cadence-ms 60000 \
    >/dev/null
./l1_diagnostics trace "$duration" live-default >/dev/null
./l1_trace_record --output "$samples" --samples 3 --cadence-ms 1000 \
    >"$profile_file"
profile=$(cat "$profile_file")
rm -f "$profile_file"
case "$profile" in
    *setup_cpu_ns=*loop_cpu_ns=*publication_cpu_ns=*total_cpu_ns=*wall_ns=*) ;;
    *) echo "FAIL: recorder profile omitted phase accounting" >&2; exit 1 ;;
esac
./l1_diagnostics trace "$samples" live-default >/dev/null

./l1_trace_record --output "$interrupted" --samples 5 --cadence-ms 1000 \
    >/dev/null 2>&1 &
recorder_pid=$!
attempt=0
while [ ! -e "$interrupted.partial" ] && kill -0 "$recorder_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 200 ]; then
        echo "FAIL: interrupt-test temporary file did not appear" >&2
        exit 1
    fi
    sleep 0.01
done
kill -TERM "$recorder_pid"
status=0
wait "$recorder_pid" || status=$?
if [ "$status" -ne 130 ] || [ -e "$interrupted" ] || \
    [ -e "$interrupted.partial" ]; then
    echo "FAIL: interrupted recorder did not fail closed and cleanly" >&2
    exit 1
fi

echo "PASS: recorder bounds, atomic no-replace publication, and duration boundary"
