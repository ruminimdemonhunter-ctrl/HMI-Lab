#!/usr/bin/env python3
import math
import statistics
import sys


if len(sys.argv) != 2 or not sys.argv[1].isdigit():
    raise SystemExit("PSI benchmark summary expected a repetition count")
expected = int(sys.argv[1])
groups: dict[tuple[str, int], list[float]] = {}
for raw in sys.stdin:
    fields = raw.strip().split(",")
    if not fields or fields[0] != "PSI_SOURCE_BENCH":
        raise SystemExit("PSI benchmark summary received unexpected input")
    values = dict(field.split("=", 1) for field in fields[1:])
    if values["valid"] != values["samples"]:
        raise SystemExit("PSI benchmark contained an invalid observation")
    key = values["mode"], int(values["samples"])
    groups.setdefault(key, []).append(float(values["cpu_ns_per_sample"]))
for (mode, samples), numbers in sorted(groups.items(), key=lambda item: (item[0][1], item[0][0])):
    if len(numbers) != expected:
        raise SystemExit("PSI benchmark repetition count mismatch")
    numbers.sort()
    p95 = numbers[math.ceil(0.95 * len(numbers)) - 1]
    print(
        "PSI_SOURCE_SUMMARY,"
        f"mode={mode},samples={samples},runs={len(numbers)},"
        f"min={numbers[0]:.3f},median={statistics.median(numbers):.3f},"
        f"p95={p95:.3f},max={numbers[-1]:.3f}"
    )
print("PASS: live PSI source profiles complete and valid")
