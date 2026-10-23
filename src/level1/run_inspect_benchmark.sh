#!/bin/sh
set -eu

rows=86400
repetitions=5
if [ "$#" -ge 1 ]; then rows=$1; fi
if [ "$#" -ge 2 ]; then repetitions=$2; fi
if [ "$#" -gt 2 ]; then
    echo "usage: sh run_inspect_benchmark.sh [ROWS] [REPETITIONS]" >&2
    exit 1
fi
trace="/tmp/hmi-l1-v09-inspect-$$.l1trace"
output="/tmp/hmi-l1-v09-inspect-$$.out"
cleanup() {
    rm -f "$trace" "$output"
}
trap cleanup EXIT HUP INT TERM

python3 evaluation/generate_inspect_trace.py "$trace" "$rows"
./l1_inspect_bench "$trace" "$repetitions"
./l1_inspect trace "$trace" --details 0 >"$output"
grep -q '^TRACE verification=complete schema=V1 two_pass_identity=matched$' "$output"
grep -q 'observer_mismatches=0' "$output"
echo "PASS: bounded inspect benchmark and verified two-pass replay"
