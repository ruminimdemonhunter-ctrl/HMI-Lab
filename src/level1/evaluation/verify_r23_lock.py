#!/usr/bin/env python3
"""Verify the committed R2.3 calibration lock without opening held-out CSVs."""

import argparse
import hashlib
import pathlib
import subprocess
import sys

CORPUS_ID = "f83a7bda572b6319554397ce780a11fa4bc0ffc698249013cedb47ec4d208963"
FILES = {
    "novelty_source_sha256": ("source", "l1_novelty.c"),
    "evaluator_source_sha256": ("source", "evaluation/l1_evaluate_r23.c"),
    "identity_novelty_test_sha256": ("source", "l1_novelty_test.c"),
    "identity_train_a_sha256": ("corpus", "machine_a_train.csv"),
    "identity_train_b_sha256": ("corpus", "machine_b_train.csv"),
    "identity_calibration_a_sha256": ("corpus", "machine_a_calibration.csv"),
    "identity_calibration_b_sha256": ("corpus", "machine_b_calibration.csv"),
    "identity_calibration_episodes_a_sha256": ("corpus", "machine_a_calibration_episodes.csv"),
    "identity_calibration_episodes_b_sha256": ("corpus", "machine_b_calibration_episodes.csv"),
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def fnv1a(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & ((1 << 64) - 1)
    return value


def parse_lock(path: pathlib.Path) -> tuple[dict[str, str], bytes]:
    raw = path.read_bytes()
    if not raw.endswith(b"\n") or b"\r" in raw:
        raise ValueError("lock must use newline-terminated LF records")
    lines = raw.splitlines(keepends=True)
    if not lines or not lines[-1].startswith(b"lock_fingerprint="):
        raise ValueError("lock fingerprint must be final")
    body = b"".join(lines[:-1])
    expected = f"{fnv1a(body):016x}".encode("ascii")
    supplied = lines[-1].removeprefix(b"lock_fingerprint=").strip()
    if supplied != expected:
        raise ValueError("lock fingerprint mismatch")
    fields: dict[str, str] = {}
    for raw_line in lines[:-1]:
        line = raw_line.decode("ascii").rstrip("\n")
        if line.count("=") != 1:
            raise ValueError("malformed lock record")
        key, value = line.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError("empty or duplicate lock record")
        fields[key] = value
    fields["lock_fingerprint"] = supplied.decode("ascii")
    return fields, raw


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", required=True, type=pathlib.Path)
    parser.add_argument("--corpus", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        fields, _ = parse_lock(args.lock)
        if fields.get("schema") != "HMI_L1_R23_CALIBRATION_LOCK_V1":
            raise ValueError("wrong lock schema")
        if fields.get("corpus_sha256") != CORPUS_ID:
            raise ValueError("wrong corpus aggregate identity")
        if sha256(args.corpus / "SHA256SUMS") != CORPUS_ID:
            raise ValueError("working corpus aggregate identity mismatch")
        if fields.get("selection_version") != "r23-c11-grid-v1":
            raise ValueError("wrong selection version")
        for key, (root, relative) in FILES.items():
            base = args.source_dir if root == "source" else args.corpus
            if fields.get(key) != sha256(base / relative):
                raise ValueError(f"identity mismatch: {key}")
        completed = subprocess.run(
            [str(args.evaluator.resolve()), "calibrate", str(args.corpus.resolve())],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        observed = hashlib.sha256(completed.stdout).hexdigest()
        if fields.get("calibration_output_sha256") != observed:
            raise ValueError("calibration output does not match committed lock")
    except (OSError, UnicodeError, ValueError, subprocess.SubprocessError) as error:
        print(f"R23_LOCK_VERIFY status=FAIL reason={error}", file=sys.stderr)
        return 1
    print(
        "R23_LOCK_VERIFY status=PASS "
        f"fingerprint={fields['lock_fingerprint']} "
        f"calibration_output_sha256={fields['calibration_output_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
