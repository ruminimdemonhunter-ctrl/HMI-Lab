#!/usr/bin/env python3
"""Generate bounded deterministic V1 traces for analyzer tests only."""

import pathlib
import sys

MAX_ROWS = 86_400
FNV_OFFSET = 1_469_598_103_934_665_603
FNV_PRIME = 1_099_511_628_211


def update_hash(value: int, data: bytes) -> int:
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & ((1 << 64) - 1)
    return value


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: generate_evidence_trace.py OUTPUT ROWS "
              "[clean|faults|no-positive|saturation]",
              file=sys.stderr)
        return 2
    output = pathlib.Path(sys.argv[1])
    try:
        rows = int(sys.argv[2], 10)
    except ValueError:
        return 2
    if rows < 2 or rows > MAX_ROWS or not output.is_absolute():
        return 2
    profile = sys.argv[3] if len(sys.argv) == 4 else "clean"
    if profile not in ("clean", "faults", "no-positive", "saturation"):
        return 2
    if profile in ("faults", "saturation") and rows < 4000:
        return 2

    header = (
        "HMI_L1_PSI_TRACE_V1\n"
        "schema=1\n"
        "units=relative_ms,basis_points,microseconds\n"
        "cadence_ms=1000\n"
        "limit=samples\n"
        f"limit_value={rows}\n"
        "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
        "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n"
    ).encode("ascii")
    checksum = update_hash(FNV_OFFSET, header)
    totals = [0, 0, 0]
    with output.open("wb") as stream:
        stream.write(header)
        for index in range(rows):
            relative_ms = index * 1000
            if profile == "faults" and index >= 1200:
                relative_ms += 7000
            high = ((relative_ms // 300_000) & 1) != 0
            if index:
                prior_high = ((((index - 1) * 1000) // 300_000) & 1) != 0
                rates = (1000, 100, 300) if prior_high else (100, 2, 10)
                if profile in ("no-positive", "saturation"):
                    rates = (100, 2, 10)
                for class_id, rate_bp in enumerate(rates):
                    totals[class_id] += rate_bp * 100
            if profile == "saturation":
                observed = (3000, 120, 800)
            elif high:
                observed = (1900, 70, 580)
            else:
                observed = (900, 45, 180)
            # Valid schema-level missing telemetry, used to exercise rejection.
            invalid = rows >= 1000 and index in (901, 2701, 5401)
            if invalid:
                line = f"D,{relative_ms},0,0,0,0,0,0,0,1\n"
                if profile == "faults" and index == 2701:
                    totals = [0, 0, 0]
            else:
                line = (
                    f"D,{relative_ms},{observed[0]},{observed[1]},{observed[2]},"
                    f"{totals[0]},{totals[1]},{totals[2]},1,0\n"
                )
            encoded = line.encode("ascii")
            stream.write(encoded)
            checksum = update_hash(checksum, encoded)
        stream.write(f"END,count={rows},fnv1a64={checksum:016x}\n".encode("ascii"))
    print(f"EVIDENCE_FIXTURE profile={profile} rows={rows} "
          f"checksum={checksum:016x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
