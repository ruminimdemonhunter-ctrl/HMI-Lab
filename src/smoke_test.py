#!/usr/bin/env python3
"""Foundation smoke test. Read-only Linux virtual machine inspection."""
from pathlib import Path
import os
import platform


def first_line(filename: str) -> str:
    try:
        return Path(filename).read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError):
        return "unavailable"


if __name__ == "__main__":
    print("HMI LAB: foundation smoke test")
    print(f"Kernel: {platform.release()}")
    print(f"Architecture: {platform.machine()}")
    print(f"Logical CPU count: {os.cpu_count()}")
    print(f"/proc/meminfo: {first_line('/proc/meminfo')}")
    print(f"CPU pressure: {first_line('/proc/pressure/cpu')}")
    print("Status: read-only telemetry available; Level 1 AI is not implemented.")
