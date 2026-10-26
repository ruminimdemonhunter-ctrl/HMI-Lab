#!/bin/sh
set -eu

for args in '0 1' '01 1' '10 1' '1 0' '1 100001' '1 -1' 'x 1'; do
    if ./l2_bench $args >/dev/null 2>&1; then
        echo "FAIL: accepted invalid benchmark arguments: $args" >&2
        exit 1
    fi
done

echo 'PASS: bounded canonical benchmark arguments'
