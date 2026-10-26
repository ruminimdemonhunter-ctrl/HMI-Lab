#!/usr/bin/env python3
"""Regenerate and byte-verify the committed R2.2 corpus."""

import argparse
import csv
import hashlib
import json
import tempfile
from pathlib import Path

from generate_r22_corpus import generate


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files(root):
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def validate_contract(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="ascii"))
    if manifest["schema"] != "HMI_L1_R22_CORPUS_V1":
        raise SystemExit("FAIL: wrong corpus schema")
    row_ids = set()
    for record in manifest["splits"]:
        with (root / record["data_file"]).open(newline="", encoding="ascii") as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != record["rows"] or record["rows"] != record["sessions"] * 320:
            raise SystemExit(f"FAIL: row count for {record['data_file']}")
        for row in rows:
            if row["row_id"] in row_ids:
                raise SystemExit(f"FAIL: duplicate row id {row['row_id']}")
            row_ids.add(row["row_id"])
        if record["split"] == "heldout":
            if record["positive_windows"] < 20 or record["normal_windows"] < 20:
                raise SystemExit("FAIL: held-out independent window support")
    sums = {}
    for line in (root / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        expected, name = line.split("  ", 1)
        sums[name] = expected
    for name, expected in sums.items():
        if digest(root / name) != expected:
            raise SystemExit(f"FAIL: checksum manifest mismatch for {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--temp-parent", type=Path)
    args = parser.parse_args()
    committed = args.corpus.resolve()
    validate_contract(committed)
    with tempfile.TemporaryDirectory(
            prefix="hmi-r22-verify-", dir=args.temp_parent) as temporary:
        generated = Path(temporary) / "corpus"
        generate(generated)
        expected_files = files(committed)
        actual_files = files(generated)
        if expected_files != actual_files:
            raise SystemExit("FAIL: corpus file inventory differs")
        for name in expected_files:
            expected = digest(committed / name)
            actual = digest(generated / name)
            if expected != actual:
                raise SystemExit(f"FAIL: {name}: {expected} != {actual}")
        identity = digest(committed / "SHA256SUMS")
        print(f"R22_CORPUS_VERIFY files={len(expected_files)} sha256sums_sha256={identity} status=PASS")


if __name__ == "__main__":
    main()
