#!/usr/bin/env python3
"""Generate a bounded synthetic V1 trace for viewer tests; never reads PSI."""

import argparse
from pathlib import Path

MAX_ROWS = 86_400
FNV_OFFSET = 1_469_598_103_934_665_603
FNV_PRIME = 1_099_511_628_211
MASK64 = (1 << 64) - 1


def update(value: int, data: bytes) -> int:
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & MASK64
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("rows", type=int)
    args = parser.parse_args()
    if not 2 <= args.rows <= MAX_ROWS:
        parser.error(f"rows must be in 2..{MAX_ROWS}")
    if not args.path.is_absolute():
        parser.error("path must be absolute")

    header = (
        "HMI_L1_PSI_TRACE_V1\n"
        "schema=1\n"
        "units=relative_ms,basis_points,microseconds\n"
        "cadence_ms=1000\n"
        "limit=samples\n"
        f"limit_value={args.rows}\n"
        "columns=relative_ms,cpu_some_bp,memory_full_bp,io_full_bp,"
        "cpu_some_total_us,memory_full_total_us,io_full_total_us,valid,flags\n"
    )
    checksum = update(FNV_OFFSET, header.encode("ascii"))
    with args.path.open("x", encoding="ascii", newline="\n") as output:
        output.write(header)
        for index in range(args.rows):
            phase = index % 40
            cpu = 3600 if 5 <= phase < 15 else 200
            memory = 180 if 15 <= phase < 22 else 5
            io = 800 if 22 <= phase < 32 else 20
            line = (
                f"D,{index * 1000},{cpu},{memory},{io},"
                f"{index * 100000},{index * 10000},{index * 20000},1,0\n"
            )
            checksum = update(checksum, line.encode("ascii"))
            output.write(line)
        output.write(f"END,count={args.rows},fnv1a64={checksum:016x}\n")


if __name__ == "__main__":
    main()
