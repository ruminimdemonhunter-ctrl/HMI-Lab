#!/bin/sh
set -eu

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
cmp testdata/l1_inspect_rising_cpu.golden /tmp/l1-inspect-golden.out
./l1_inspect demo --details 0 >/tmp/l1-inspect-all.out
grep '^SUMMARY ' /tmp/l1-inspect-all.out >/tmp/l1-inspect-summaries.out
cmp testdata/l1_inspect_summaries.golden /tmp/l1-inspect-summaries.out
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
rm -f /tmp/l1-inspect-golden.out
rm -f /tmp/l1-inspect-all.out /tmp/l1-inspect-summaries.out
echo "PASS: inspect argument, path, option, and output bounds"
