#!/bin/sh
set -eu
for file in l2_state.c l2_state.h; do
    if grep -En '\b(malloc|calloc|realloc|free|fopen|open|read|write|system|exec|fork|socket|connect|clock_gettime)\b' "$file"; then
        echo "FAIL: forbidden core API in $file" >&2
        exit 1
    fi
done
if grep -ERn 'programmer|artist|occupation|profession|/proc|/sys|cgroup|scheduler|firmware' -- *.c *.h; then
    echo 'FAIL: forbidden identity/OS-control literal' >&2
    exit 1
fi
echo 'PASS: L2 pure-core and no-profile/no-actuation source audit'
