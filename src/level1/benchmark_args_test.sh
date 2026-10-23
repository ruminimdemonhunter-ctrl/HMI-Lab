#!/bin/sh
set -eu

./l1_bench core fixed 1 >/dev/null
./l1_shadow_bench core l1-shadow 1 >/dev/null

for invalid in 0 +1 -1 1x 1000000001 18446744073709551616; do
    if ./l1_bench core fixed "$invalid" >/dev/null 2>&1; then
        echo "FAIL: accepted invalid core count: $invalid" >&2
        exit 1
    fi
done

for invalid in 0 +1 -1 1x 1000000001 18446744073709551616; do
    if ./l1_shadow_bench core l1-shadow "$invalid" >/dev/null 2>&1; then
        echo "FAIL: shadow benchmark accepted invalid core count: $invalid" >&2
        exit 1
    fi
done

if ./l1_bench live fixed 1000001 >/dev/null 2>&1; then
    echo "FAIL: accepted live count above limit" >&2
    exit 1
fi

if ./l1_shadow_bench live l1 1000001 >/dev/null 2>&1; then
    echo "FAIL: shadow benchmark accepted live count above limit" >&2
    exit 1
fi

if ./l1_bench unknown fixed 1 >/dev/null 2>&1 ||
   ./l1_bench core unknown 1 >/dev/null 2>&1; then
    echo "FAIL: accepted invalid benchmark mode or engine" >&2
    exit 1
fi

if ./l1_shadow_bench unknown l1 1 >/dev/null 2>&1 ||
   ./l1_shadow_bench core unknown 1 >/dev/null 2>&1; then
    echo "FAIL: accepted invalid shadow benchmark mode or engine" >&2
    exit 1
fi

echo "PASS: benchmark argument grammar, overflow, modes, and sample limits"
