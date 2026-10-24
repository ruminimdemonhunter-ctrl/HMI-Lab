#!/bin/sh
set -eu
trace="/tmp/l1-inspect-args-$$.l1trace"
corrupt="/tmp/l1-inspect-args-$$.corrupt.l1trace"
cleanup() {
    rm -f "$trace" "$corrupt"
}
trap cleanup EXIT HUP INT TERM

expect_fail() {
    if "$@" >/tmp/l1-inspect-args.out 2>/tmp/l1-inspect-args.err; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

./l1_inspect help >/tmp/l1-inspect-help.out
grep -q '^usage:$' /tmp/l1-inspect-help.out
./l1_inspect demo rising-cpu --details 0 >/tmp/l1-inspect-summary.out
grep -q '^SUMMARY name=rising-cpu rows=7 ' /tmp/l1-inspect-summary.out
grep -q 'observer_mismatches=0' /tmp/l1-inspect-summary.out
./l1_inspect demo rising-cpu >/tmp/l1-inspect-golden.out
sed 's/\r$//' testdata/l1_inspect_rising_cpu.golden \
    >/tmp/l1-inspect-golden.expected
cmp /tmp/l1-inspect-golden.expected /tmp/l1-inspect-golden.out
./l1_inspect demo --details 0 >/tmp/l1-inspect-all.out
grep '^SUMMARY ' /tmp/l1-inspect-all.out >/tmp/l1-inspect-summaries.out
sed 's/\r$//' testdata/l1_inspect_summaries.golden \
    >/tmp/l1-inspect-summaries.expected
cmp /tmp/l1-inspect-summaries.expected /tmp/l1-inspect-summaries.out
python3 evaluation/generate_inspect_trace.py "$trace" 2
./l1_inspect trace "$trace" --forecast-config live-default --details 2 \
    >/tmp/l1-inspect-trace-live.out
./l1_inspect trace "$trace" --forecast-config v06-trained --details 2 \
    >/tmp/l1-inspect-trace-v06.out
grep -q '^TRACE verification=complete schema=V1 two_pass_identity=matched$' \
    /tmp/l1-inspect-trace-live.out
grep -q 'forecast_config=v06-trained' /tmp/l1-inspect-trace-v06.out
if grep -F "$trace" /tmp/l1-inspect-trace-live.out; then
    echo "private trace path leaked to output" >&2
    exit 1
fi
sed 's/^END,count=2/END,count=3/' "$trace" >"$corrupt"
expect_fail ./l1_inspect trace "$corrupt"
expect_fail ./l1_inspect
expect_fail ./l1_inspect unknown
expect_fail ./l1_inspect demo not-a-case
expect_fail ./l1_inspect demo rising-cpu --details 2001
expect_fail ./l1_inspect demo rising-cpu --details -1
expect_fail ./l1_inspect demo rising-cpu --unknown
expect_fail ./l1_inspect trace relative.trace
expect_fail ./l1_inspect trace /tmp/../tmp/example.trace
expect_fail ./l1_inspect trace /tmp/missing.trace --forecast-config unknown
expect_fail ./l1_inspect trace /tmp/missing.trace --details 2001

rm -f /tmp/l1-inspect-args.out /tmp/l1-inspect-args.err \
      /tmp/l1-inspect-help.out /tmp/l1-inspect-summary.out
rm -f /tmp/l1-inspect-golden.out /tmp/l1-inspect-golden.expected
rm -f /tmp/l1-inspect-all.out /tmp/l1-inspect-summaries.out \
      /tmp/l1-inspect-summaries.expected
rm -f /tmp/l1-inspect-trace-live.out /tmp/l1-inspect-trace-v06.out \
      "$trace" "$corrupt"
echo "PASS: inspect argument, path, option, and output bounds"
