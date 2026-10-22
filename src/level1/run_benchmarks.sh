#!/bin/sh
set -eu

repetitions=${1:-5}
live_samples=${2:-10000}
core_samples=${3:-10000000}

case "$repetitions:$live_samples:$core_samples" in
    *[!0-9:]*|0:*|*:0:*|*:0) echo "usage: $0 [positive repetitions] [positive live samples] [positive core samples]" >&2; exit 2 ;;
esac

echo "# uname: $(uname -srvmo)"
echo "# compiler: $(cc --version | sed -n '1p')"
echo "# repetitions=$repetitions live_samples=$live_samples core_samples=$core_samples"
i=1
while [ "$i" -le "$repetitions" ]; do
    echo "# repetition=$i"
    ./l1_bench core fixed "$core_samples"
    ./l1_bench core l1 "$core_samples"
    ./l1_bench live fixed "$live_samples"
    ./l1_bench live l1 "$live_samples"
    i=$((i + 1))
done
