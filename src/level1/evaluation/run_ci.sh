#!/bin/sh
set -eu

policy="${TMPDIR:-/tmp}/l1-v04-ci-$$.eval.policy"
trap 'rm -f "$policy"' EXIT HUP INT TERM

./l1_evaluate development
./l1_evaluate train "$policy"
./l1_evaluate heldout "$policy" 1
