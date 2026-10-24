#!/bin/sh
set -eu

valid="/tmp/l1-proposal-valid-$$.txt"
malformed="/tmp/l1-proposal-malformed-$$.txt"
output="/tmp/l1-proposal-output-$$.txt"
cleanup() { rm -f "$valid" "$malformed" "$output"; }
trap cleanup EXIT HUP INT TERM

expect_fail() {
    if "$@" >/dev/null 2>/dev/null; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

cp policy_proposal.example "$valid"
chmod 600 "$valid"
./l1_proposal_check "$valid" --current-generation 0 >"$output"
grep -q '^PROPOSAL envelope=canonical schema=1 base_generation=0 generation=1 safety=PASS generation_check=PASS ' "$output"
grep -q '^AUTHORIZATION verdict=DENY reason=authorization-unavailable verifier=none test_only=no authority=none production_apply=blocked$' "$output"
grep -q '^PROPOSAL_VALIDATION:PASS_OFFLINE_ONLY$' "$output"
grep -q '^PRODUCTION_AUTHORIZATION:BLOCKED$' "$output"
grep -q '^L2_DEPLOYMENT:NOT_AUTHORIZED$' "$output"
if grep -F "$valid" "$output"; then
    echo "private proposal path leaked" >&2
    exit 1
fi

expect_fail ./l1_proposal_check "$valid" --current-generation 1
expect_fail ./l1_proposal_check "$valid" --current-generation 00
expect_fail ./l1_proposal_check "$valid" --current-generation 4294967296
expect_fail ./l1_proposal_check "$valid" --authorize
expect_fail ./l1_proposal_check "$valid"
expect_fail ./l1_proposal_check relative --current-generation 0

printf '%s\n' 'authorized=true' >"$malformed"
chmod 600 "$malformed"
expect_fail ./l1_proposal_check "$malformed" --current-generation 0

echo "PASS: proposal CLI is bounded, offline-only, and authorization-denied"
