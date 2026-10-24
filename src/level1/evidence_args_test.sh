#!/bin/sh
set -eu

fixture="/tmp/l1-evidence-fixture-$$.l1trace"
short="/tmp/l1-evidence-short-$$.l1trace"
tiny="/tmp/l1-evidence-tiny-$$.l1trace"
faults="/tmp/l1-evidence-faults-$$.l1trace"
no_positive="/tmp/l1-evidence-no-positive-$$.l1trace"
saturation="/tmp/l1-evidence-saturation-$$.l1trace"
corrupt="/tmp/l1-evidence-corrupt-$$.l1trace"
output="/tmp/l1-evidence-output-$$.txt"
mislabeled="/tmp/l1-evidence-mislabeled-$$.txt"
selected="/tmp/l1-evidence-selected-$$.txt"
golden="/tmp/l1-evidence-golden-$$.expected"
cleanup() {
    rm -f "$fixture" "$short" "$tiny" "$faults" "$no_positive" \
        "$saturation" "$corrupt" "$output" "$mislabeled" "$selected" "$golden"
}
trap cleanup EXIT HUP INT TERM

expect_fail() {
    if "$@" >/dev/null 2>/dev/null; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

python3 evaluation/generate_evidence_trace.py "$fixture" 7201
python3 evaluation/generate_evidence_trace.py "$short" 65
python3 evaluation/generate_evidence_trace.py "$tiny" 5
python3 evaluation/generate_evidence_trace.py "$faults" 7201 faults
python3 evaluation/generate_evidence_trace.py "$no_positive" 7201 no-positive
python3 evaluation/generate_evidence_trace.py "$saturation" 7201 saturation
./l1_evidence analyze "$fixture" --source synthetic-fixture >"$output"

grep -E '^(TRACE|SPLIT|WINDOWS split=evaluation|CLASS split=evaluation|SAFETY|COVERAGE|EVIDENCE_TOOLING|REAL_EVIDENCE|FORECAST_TRANSFER|ADAPTATION|ACTION_COUPLING|REAL_RESOURCE_BENEFIT)' \
    "$output" >"$selected"
sed 's/\r$//' testdata/l1_evidence_fixture.golden \
    >"$golden"
cmp "$golden" "$selected"
rm -f "$golden"
grep -q '^EVIDENCE_TOOLING:GO$' "$output"
grep -q '^CAPTURE_PROVENANCE status=not-applicable source=synthetic-fixture$' "$output"
grep -q '^REAL_EVIDENCE:INSUFFICIENT reason=synthetic-fixture-only$' "$output"
grep -q '^SAFETY accounting=PASS core_action_byte_equivalence=PASS ' "$output"
if grep -F "$fixture" "$output"; then
    echo "private trace path leaked" >&2
    exit 1
fi

# A source label is a caller claim, not independent capture provenance.
./l1_evidence analyze "$fixture" --source real-guest-v1 >"$mislabeled"
grep -q '^COVERAGE .* gate=PASS$' "$mislabeled"
grep -q '^CAPTURE_PROVENANCE status=unverified source=caller-declared authorization=unverified independent_review=required$' "$mislabeled"
grep -q '^REAL_EVIDENCE:INSUFFICIENT reason=unverified-provenance$' "$mislabeled"
if grep -q '^REAL_EVIDENCE:READY$' "$mislabeled"; then
    echo "caller-declared source established readiness" >&2
    exit 1
fi
if grep -F "$fixture" "$mislabeled"; then
    echo "private trace path leaked" >&2
    exit 1
fi

# A short valid fixture analyzes safely but cannot pass the scientific gate.
./l1_evidence analyze "$short" --source synthetic-fixture >"$output"
grep -q '^COVERAGE .* gate=FAIL$' "$output"
./l1_evidence analyze "$tiny" --source synthetic-fixture >"$output"
grep -q '^COVERAGE evaluation_windows=0 .* gate=FAIL$' "$output"

# Valid schema-level faults exercise exact feedback rejection categories.
./l1_evidence analyze "$faults" --source synthetic-fixture >"$output"
grep -Eq '^WINDOWS .* invalid=[1-9][0-9]* .*gap_late=[1-9][0-9]* .*shadow_resets=[1-9][0-9]* ' "$output"

# A valid one-class-empty trace must never pass the population gate.
./l1_evidence analyze "$no_positive" --source synthetic-fixture >"$output"
grep -q '^CLASS split=evaluation .* positives=0 ' "$output"
grep -q '^COVERAGE .* gate=FAIL$' "$output"

# A bounded false-alarm fixture reaches every offset bound and exercises
# further attempted changes without modifying the learner.
./l1_evidence analyze "$saturation" --source synthetic-fixture >"$output"
awk '
    /^CLASS split=evaluation / {
        saturated = 0
        attempts = 0
        first = ""
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^saturated_offset_windows=/) {
                split($i, value, "="); saturated = value[2] + 0
            }
            if ($i ~ /^saturation_attempts=/) {
                split($i, value, "="); attempts = value[2] + 0
            }
            if ($i ~ /^first_scored_material_change_elapsed_ms=/) {
                split($i, value, "="); first = value[2]
            }
        }
        if (saturated < 1 || attempts < 1 || first != "N/A") exit 1
        classes++
    }
    END { if (classes != 6) exit 1 }
' "$output"

sed 's/fnv1a64=./fnv1a64=0/' "$fixture" >"$corrupt"
expect_fail ./l1_evidence analyze "$corrupt" --source synthetic-fixture
expect_fail ./l1_evidence analyze relative.l1trace --source synthetic-fixture
expect_fail ./l1_evidence analyze /tmp/../tmp/x --source synthetic-fixture
expect_fail ./l1_evidence analyze "$fixture" --source unknown
expect_fail ./l1_evidence analyze "$fixture"
expect_fail ./l1_evidence
expect_fail python3 evaluation/generate_evidence_trace.py "$fixture" 1
expect_fail python3 evaluation/generate_evidence_trace.py "$fixture" 86401

echo "PASS: evidence provenance, diagnostics, golden output, parser negatives, and bounds"
