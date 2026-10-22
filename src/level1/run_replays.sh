#!/bin/sh
set -eu

repetitions=${1:-5}
inner_repetitions=${2:-10000}

case "$repetitions:$inner_repetitions" in
    *[!0-9:]*|0:*|*:0) echo "usage: $0 [positive runs] [replays per run]" >&2; exit 2 ;;
esac

echo "# uname: $(uname -srvmo)"
echo "# compiler: $(cc --version | sed -n '1p')"
echo "# runs=$repetitions replays_per_run=$inner_repetitions"
i=1
while [ "$i" -le "$repetitions" ]; do
    echo "# run=$i"
    ./l1_replay "$inner_repetitions"
    i=$((i + 1))
done
