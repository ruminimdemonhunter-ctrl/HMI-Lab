#!/bin/sh
set -eu

manifest="${TMPDIR:-/tmp}/l1-v06-ci-$$.eval.manifest"
trap 'rm -f "$manifest"' EXIT HUP INT TERM

./l1_evaluate_v06 development
./l1_evaluate_v06 train "$manifest"
./l1_evaluate_v06 heldout "$manifest" 1
