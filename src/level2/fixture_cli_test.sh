#!/bin/sh
set -eu
root=./evaluation/fixtures_v1
a="$(./l2_fixture_cli "$root/a.csv")"
b="$(./l2_fixture_cli "$root/b.csv")"
mixed="$(./l2_fixture_cli "$root/mixed.csv")"
cold="$(./l2_fixture_cli "$root/cold.csv")"
reset="$(./l2_fixture_cli "$root/reset_late.csv")"
pressure="$(./l2_fixture_cli "$root/pressure_block.csv")"
printf '%s\n' "$a" | grep -q 'status=CANDIDATE.*roles=CODE_EDIT|BUILD_TEST|DOC_VIEW'
printf '%s\n' "$b" | grep -q 'status=CANDIDATE.*roles=REFERENCES|CANVAS|ASSET_GROUP'
printf '%s\n' "$mixed" | grep -q 'workspace=1 status=CANDIDATE.*roles=CODE_EDIT|BUILD_TEST|DOC_VIEW'
printf '%s\n' "$mixed" | grep -q 'workspace=2 status=CANDIDATE.*roles=REFERENCES|CANVAS|ASSET_GROUP'
printf '%s\n' "$cold" | grep -q 'status=NONE.*reason=WARMING_UP'
printf '%s\n' "$reset" | grep -q 'status=NONE.*reason=WARMING_UP'
printf '%s\n' "$pressure" | grep -q 'status=BLOCKED.*reason=RESOURCE_CONSTRAINED'
if ./l2_fixture_cli "$root/bad_provenance.csv" >/dev/null 2>&1; then
    echo 'FAIL: bad provenance accepted' >&2
    exit 1
fi
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
printf '%s\n' 'schema,seq,monotonic_ms,quality,event,workspace,role,activity,l1_condition,provenance' >"$tmp"
i=0
while [ "$i" -lt 257 ]; do
    printf x >>"$tmp"
    i=$((i + 1))
done
printf '\n' >>"$tmp"
if ./l2_fixture_cli "$tmp" >/dev/null 2>&1; then
    echo 'FAIL: oversized row accepted' >&2
    exit 1
fi
printf '%s\n' 'not,the,frozen,header' >"$tmp"
if ./l2_fixture_cli "$tmp" >/dev/null 2>&1; then
    echo 'FAIL: malformed header accepted' >&2
    exit 1
fi
test "$(printf '%s\n' "$a" | grep -c '^L2_QUERY')" -eq 1
echo 'PASS: offline fixture CLI A/B/mixed/abstention/safety and bounds outputs'
