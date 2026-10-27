#!/usr/bin/env python3
"""Canonical SHA-256 lock and first-open guard for the V0.2a toy preflight."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
SENTINEL = HERE / "dummy_sentinel.txt"
BOUND = [
    "src/level2/l2_state.c",
    "src/level2/l2_state.h",
    "src/level2/l2_parser.c",
    "src/level2/evaluation/fixtures_v1/SHA256SUMS",
    "src/level2/evaluation/v02a/CORPUS_SHA256SUMS",
    "src/level2/evaluation/v02a/generate_preflight_fixtures.py",
    "src/level2/evaluation/v02a/verify_preflight_fixtures.py",
    "src/level2/evaluation/v02a/baseline_preflight.py",
    "src/level2/evaluation/v02a/toy_scorer.py",
    "src/level2/evaluation/v02a/lock_preflight.py",
    "src/level2/evaluation/v02a/freeze_mock_lock.py",
    "src/level2/evaluation/v02a/preflight_runner.py",
    "src/level2/evaluation/v02a/fixtures/SHA256SUMS",
    "src/level2/evaluation/v02a/fixtures/metadata.csv",
    "src/level2/evaluation/v02a/fixtures/truth.csv",
    "src/level2/evaluation/v02a/fixtures/query_context.csv",
    "src/level2/evaluation/v02a/fixtures/training.csv",
]


class LockInvalid(ValueError):
    pass


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ordered_values(predictions: Path, selection_digest: str) -> list[tuple[str, str]]:
    values = [("schema", "HMI_L2_V02A_LOCK_V1"),
              ("mode", "TOY_PREFLIGHT_ONLY"),
              ("grid_recent_n", "1|2|3|5|8"),
              ("selected_cal_recent", "RECENT_1"),
              ("selected_cal_static", "STATIC_WORKSPACE"),
              ("selected_cal_none", "ALWAYS_NONE"),
              ("support_rule", "sessions:3+|transitions:6+|triple:2+"),
              ("guard_rule", "INVALID_NONE|DENY_BLOCKED|PRESSURE_BLOCKED"),
              ("selection_digest", selection_digest),
              ("predictions_sha256", sha(predictions.read_bytes()))]
    values.extend((f"file.{path}.sha256", sha((REPO / path).read_bytes())) for path in BOUND)
    return values


def write_lock(path: Path, predictions: Path, selection_digest: str) -> None:
    rows = ordered_values(predictions, selection_digest)
    path.write_text("".join(f"{key}={value}\n" for key, value in rows),
                    encoding="ascii", newline="")


def parse_lock(data: bytes) -> list[tuple[str, str]]:
    if not data or len(data) > 16384 or not data.endswith(b"\n") or any(
            byte < 0x20 and byte != 0x0A or byte > 0x7E for byte in data):
        raise LockInvalid("lock bytes")
    rows = []
    for line in data.decode("ascii").splitlines():
        if line.count("=") != 1:
            raise LockInvalid("lock grammar")
        key, value = line.split("=", 1)
        if not key or not value or any(existing == key for existing, _ in rows):
            raise LockInvalid("lock key")
        rows.append((key, value))
    return rows


def verify(lock_path: Path, predictions: Path, selection_digest: str,
           reader: Callable[[Path], bytes] = lambda path: path.read_bytes()) -> None:
    actual = parse_lock(reader(lock_path))
    expected = ordered_values(predictions, selection_digest)
    if [key for key, _ in actual] != [key for key, _ in expected]:
        raise LockInvalid("missing/unknown/reordered key")
    for (actual_key, actual_value), (expected_key, expected_value) in zip(actual, expected):
        if actual_key != expected_key:
            raise LockInvalid("key")
        if actual_key.startswith("file."):
            relative = actual_key[5:-7]
            expected_value = sha(reader(REPO / relative))
        if actual_value != expected_value:
            raise LockInvalid("value")


def verify_then_open(lock_path: Path, predictions: Path, selection_digest: str,
                     requested: Path, attempts: list[Path],
                     reader: Callable[[Path], bytes] = lambda path: path.read_bytes()) -> bytes:
    verify(lock_path, predictions, selection_digest, reader)
    if requested.resolve() != SENTINEL.resolve():
        raise LockInvalid("unspecified path")
    attempts.append(requested)
    return reader(requested)
