#!/bin/sh
set -eu

manifest="${TMPDIR:-/tmp}/l1-v05-ci-$$.eval.manifest"
trap 'rm -f "$manifest"' EXIT HUP INT TERM

./l1_evaluate_v05 development
./l1_evaluate_v05 train "$manifest"
./l1_evaluate_v05 heldout "$manifest" 1
