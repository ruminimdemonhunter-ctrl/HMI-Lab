#!/usr/bin/env python3
import math
import statistics
import sys


def fail(message: str) -> None:
    raise SystemExit(f"recorder benchmark summary failed: {message}")


if len(sys.argv) != 2 or not sys.argv[1].isdigit():
    fail("expected repetition count")
expected = int(sys.argv[1])
groups: dict[tuple[str, int], list[dict[str, str]]] = {}
for raw in sys.stdin:
    line = raw.strip()
    if not line:
        continue
    fields = line.split(",")
    if fields[0] != "RECORDER_BENCH":
        fail("unexpected input")
    values = dict(field.split("=", 1) for field in fields[1:])
    key = values["mode"], int(values["samples"])
    groups.setdefault(key, []).append(values)

metrics = ("total_cpu_ns_per_sample", "hot_cpu_ns_per_sample", "rss_kib")
for key in sorted(groups, key=lambda item: (item[1], item[0])):
    rows = groups[key]
    if len(rows) != expected:
        fail(f"{key} has {len(rows)} runs, expected {expected}")
    mode, samples = key
    for metric in metrics:
        numbers = sorted(float(row[metric]) for row in rows)
        p95 = numbers[math.ceil(0.95 * len(numbers)) - 1]
        print(
            "RECORDER_SUMMARY,"
            f"mode={mode},samples={samples},metric={metric},runs={len(numbers)},"
            f"min={numbers[0]:.3f},median={statistics.median(numbers):.3f},"
            f"p95={p95:.3f},max={numbers[-1]:.3f}"
        )

for samples in sorted({key[1] for key in groups}):
    baseline = groups.get(("baseline", samples), [])
    candidate = groups.get(("candidate", samples), [])
    if baseline and candidate:
        expected_hashes = {row["checksum"] for row in baseline + candidate}
        if len(expected_hashes) != 1:
            fail(f"checksum mismatch at {samples} samples")
print("PASS: recorder benchmark repetitions and checksums verified")
