#!/bin/sh
set -eu

rows=${1:-86400}
repetitions=${2:-5}
mode=${3:-optimized}
case "$rows" in *[!0-9]*|'') exit 2;; esac
case "$repetitions" in *[!0-9]*|'') exit 2;; esac
[ "$rows" -ge 2 ] && [ "$rows" -le 86400 ] || exit 2
[ "$repetitions" -ge 1 ] && [ "$repetitions" -le 5 ] || exit 2
case "$mode" in optimized|instrumented) ;; *) exit 2;; esac

trace="/tmp/l1-evidence-bench-$$.l1trace"
cleanup() { rm -f "$trace"; }
trap cleanup EXIT HUP INT TERM
python3 evaluation/generate_evidence_trace.py "$trace" "$rows"
if [ "$mode" = instrumented ]; then
    ./l1_evidence_bench "$trace" "$repetitions" "$rows" instrumented
else
    ./l1_evidence_bench "$trace" "$repetitions" "$rows"
fi
