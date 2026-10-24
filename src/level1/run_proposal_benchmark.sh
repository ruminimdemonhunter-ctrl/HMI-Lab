#!/bin/sh
set -eu

[ "$#" -eq 2 ] || exit 2
repetitions=$1
mode=$2
case "$repetitions" in *[!0-9]*|'') exit 2;; esac
[ "$repetitions" -ge 1 ] && [ "$repetitions" -le 5 ] || exit 2
case "$mode" in optimized|instrumented) ;; *) exit 2;; esac

valid="/tmp/l1-proposal-bench-valid-$$.txt"
negative="/tmp/l1-proposal-bench-negative-$$.txt"
cleanup() { rm -f "$valid" "$negative"; }
trap cleanup EXIT HUP INT TERM
cp policy_proposal.example "$valid"
chmod 600 "$valid"
python3 -c "import pathlib; pathlib.Path('$negative').write_bytes(b'X' * 4096)"
chmod 600 "$negative"
if [ "$mode" = instrumented ]; then
    ./l1_proposal_bench "$valid" "$negative" "$repetitions" instrumented
else
    ./l1_proposal_bench "$valid" "$negative" "$repetitions"
fi
