#!/bin/sh
set -eu

repetitions=${1:-5}
population=${2:-full}
case "$repetitions" in
    *[!0-9]*|0|'') echo "repetitions must be 1..10" >&2; exit 2 ;;
esac
if [ "$repetitions" -gt 10 ]; then
    echo "repetitions must be 1..10" >&2
    exit 2
fi
case "$population" in
    full) counts="5 65 180" ;;
    ci) counts="5 65" ;;
    *) echo "population must be full or ci" >&2; exit 2 ;;
esac

results=$(mktemp /tmp/hmi-l1-recorder-results-XXXXXX)
trap 'rm -f "$results"' EXIT HUP INT TERM

echo "# uname: $(uname -srvmo)"
echo "# compiler: $(cc --version | sed -n '1p')"
echo "# loadavg: $(sed -n '1p' /proc/loadavg)"
echo "# repetitions=$repetitions population=$population counts=$counts"
for count in $counts; do
    repetition=1
    while [ "$repetition" -le "$repetitions" ]; do
        if [ $((repetition % 2)) -eq 1 ]; then
            order="baseline candidate"
        else
            order="candidate baseline"
        fi
        for mode in $order; do
            ./l1_recorder_bench "$mode" "$count" | tee -a "$results"
        done
        repetition=$((repetition + 1))
    done
done
python3 ./evaluation/summarize_recorder_bench.py "$repetitions" <"$results"
