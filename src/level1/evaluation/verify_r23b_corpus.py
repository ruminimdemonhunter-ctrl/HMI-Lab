#!/usr/bin/env python3
"""Independently verify R2.3b corpus V2 identities and post-fault eligibility."""

import argparse
import csv
import hashlib
import json
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

from generate_r23b_corpus import generate

SCHEMA = "HMI_L1_R23B_CORPUS_V2"
SPLIT_EXPECTED = {"train": (24, 0), "calibration": (8, 24), "heldout": (8, 32)}
ROW_FIELDS = ("schema", "row_id", "machine", "split", "session_id",
              "row_in_session", "monotonic_ms", "valid", "quality",
              "cpu_some_bp", "memory_full_bp", "io_full_bp",
              "scoring_class", "event_family", "episode_id", "exclude_reason")
EPISODE_FIELDS = ("schema", "machine", "split", "session_id", "episode_id",
                  "scoring_class", "event_family", "start_row", "end_row",
                  "eligible_rows")


class ContractError(ValueError):
    pass


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strict_uint(text, limit):
    if not text or (len(text) > 1 and text[0] == "0") or not text.isascii() or not text.isdigit():
        raise ContractError(f"invalid unsigned integer {text!r}")
    value = int(text)
    if value > limit:
        raise ContractError(f"unsigned integer exceeds {limit}: {text}")
    return value


def load_csv(path, expected_fields):
    with path.open(newline="", encoding="ascii") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != expected_fields:
            raise ContractError(f"wrong columns: {path.name}")
        return list(reader)


def eligible(row, previous_timestamp):
    valid = strict_uint(row["valid"], 1) == 1
    values = [strict_uint(row[name], 10001) for name in
              ("cpu_some_bp", "memory_full_bp", "io_full_bp")]
    timestamp = strict_uint(row["monotonic_ms"], (1 << 63) - 1)
    monotonic = previous_timestamp is None or timestamp > previous_timestamp
    return (valid and row["quality"] == "OK" and all(value <= 10000 for value in values)
            and monotonic and row["exclude_reason"] == "")


def validate_episode_records(rows, episodes, machine, split):
    by_session = defaultdict(list)
    for row in rows:
        by_session[strict_uint(row["session_id"], 39)].append(row)
    occupied = defaultdict(set)
    class_counts = Counter()
    family_counts = Counter()
    for episode in episodes:
        if episode["schema"] != SCHEMA or episode["machine"] != machine or episode["split"] != split:
            raise ContractError("episode identity mismatch")
        session = strict_uint(episode["session_id"], 39)
        start = strict_uint(episode["start_row"], 319)
        end = strict_uint(episode["end_row"], 319)
        declared = strict_uint(episode["eligible_rows"], 320)
        if start > end or end - start + 1 < 10:
            raise ContractError(f"invalid episode range {episode['episode_id']}")
        span = set(range(start, end + 1))
        if occupied[session].intersection(span):
            raise ContractError(f"overlapping episode {episode['episode_id']}")
        occupied[session].update(span)
        selected = [row for row in by_session[session]
                    if start <= strict_uint(row["row_in_session"], 319) <= end]
        if len(selected) != end - start + 1:
            raise ContractError(f"missing rows in {episode['episode_id']}")
        previous = None
        actual = 0
        for row in selected:
            if (row["episode_id"] != episode["episode_id"] or
                    row["scoring_class"] != episode["scoring_class"] or
                    row["event_family"] != episode["event_family"]):
                raise ContractError(f"row/episode mismatch {episode['episode_id']}")
            if eligible(row, previous):
                actual += 1
                previous = int(row["monotonic_ms"])
        if actual != declared:
            raise ContractError(
                f"post-exclusion eligibility mismatch {episode['episode_id']}: "
                f"declared={declared} actual={actual}")
        if actual >= 10:
            class_counts[episode["scoring_class"]] += 1
            family_counts[episode["event_family"]] += 1
    return class_counts, family_counts


def validate_split(root, record, all_row_ids):
    machine, split = record["machine"], record["split"]
    if machine not in ("A", "B") or split not in SPLIT_EXPECTED:
        raise ContractError("unknown split identity")
    rows = load_csv(root / record["data_file"], ROW_FIELDS)
    episodes = load_csv(root / record["episode_file"], EPISODE_FIELDS)
    expected_sessions, first_session = SPLIT_EXPECTED[split]
    if len(rows) != expected_sessions * 320 or len(episodes) != expected_sessions * 7:
        raise ContractError(f"wrong row/episode count {machine} {split}")
    sessions = defaultdict(list)
    for row in rows:
        if row["schema"] != SCHEMA or row["machine"] != machine or row["split"] != split:
            raise ContractError("row identity mismatch")
        if row["row_id"] in all_row_ids:
            raise ContractError(f"duplicate row id {row['row_id']}")
        all_row_ids.add(row["row_id"])
        session = strict_uint(row["session_id"], 39)
        sessions[session].append(row)
        # Invalid observations must be explicit; valid observations cannot be silently excluded.
        is_intrinsically_valid = (row["quality"] == "OK" and row["valid"] == "1" and
                                  all(strict_uint(row[name], 10001) <= 10000 for name in
                                      ("cpu_some_bp", "memory_full_bp", "io_full_bp")))
        explicitly_time_invalid = row["exclude_reason"] == "time_regression"
        if is_intrinsically_valid != (row["exclude_reason"] == "") and not \
                (is_intrinsically_valid and explicitly_time_invalid):
            raise ContractError(f"exclude reason mismatch {row['row_id']}")
    expected_ids = set(range(first_session, first_session + expected_sessions))
    if set(sessions) != expected_ids:
        raise ContractError(f"session identity mismatch {machine} {split}")
    for session, session_rows in sessions.items():
        if len(session_rows) != 320:
            raise ContractError(f"session row count {session}")
        indices = [strict_uint(row["row_in_session"], 319) for row in session_rows]
        if indices != list(range(320)):
            raise ContractError(f"row ordering {session}")
        prior = None
        for row in session_rows:
            timestamp = strict_uint(row["monotonic_ms"], (1 << 63) - 1)
            regressed = prior is not None and timestamp <= prior
            if regressed != (row["exclude_reason"] == "time_regression"):
                raise ContractError(f"timestamp regression marking {row['row_id']}")
            prior = max(prior, timestamp) if prior is not None else timestamp
    counts, families = validate_episode_records(rows, episodes, machine, split)
    if counts["normal"] != record["normal_episodes"] or \
            counts["positive"] != record["positive_episodes"]:
        raise ContractError(f"manifest support mismatch {machine} {split}")
    if split == "heldout":
        if counts["normal"] < 20 or counts["positive"] < 20:
            raise ContractError(f"insufficient heldout support {machine}")
        if max((count for family, count in families.items()
                if family != "operational_baseline"), default=0) < 5:
            raise ContractError(f"no heldout family support {machine}")
    return counts, families, rows, episodes


def validate_probes(root):
    fields = ("schema", "probe_id", "row", "monotonic_ms", "valid", "quality",
              "cpu_some_bp", "memory_full_bp", "io_full_bp", "exclude_reason")
    truth_fields = ("schema", "probe_id", "machine", "truth_label", "truth_origin")
    rows = load_csv(root / "contextual_probes.csv", fields)
    truths = load_csv(root / "contextual_probe_truth.csv", truth_fields)
    grouped = defaultdict(list)
    for row in rows:
        if row["schema"] != SCHEMA:
            raise ContractError("probe schema")
        grouped[row["probe_id"]].append(row)
    if set(grouped) != {"A_NATIVE", "B_NATIVE", "INVALID_IDENTICAL"} or \
            any(len(values) != 40 for values in grouped.values()):
        raise ContractError("probe row inventory")
    truth = {(row["probe_id"], row["machine"]): row["truth_label"] for row in truths}
    expected = {
        ("A_NATIVE", "A"): "EXPECTED", ("A_NATIVE", "B"): "SUSPECTED",
        ("B_NATIVE", "A"): "SUSPECTED", ("B_NATIVE", "B"): "EXPECTED",
        ("INVALID_IDENTICAL", "A"): "UNKNOWN", ("INVALID_IDENTICAL", "B"): "UNKNOWN",
    }
    if truth != expected:
        raise ContractError("machine-conditioned probe truth mismatch")
    # Each canonical sequence is intentionally evaluated unchanged against both memories.
    for probe in ("A_NATIVE", "B_NATIVE"):
        indices = [strict_uint(row["row"], 39) for row in grouped[probe]]
        if indices != list(range(40)) or any(row["quality"] != "OK" for row in grouped[probe]):
            raise ContractError(f"invalid canonical probe {probe}")
    if any(row["quality"] == "OK" or row["valid"] != "0"
           for row in grouped["INVALID_IDENTICAL"]):
        raise ContractError("invalid probe is not uniformly invalid")
    return {probe: hashlib.sha256("\n".join(
        ",".join(row[key] for key in fields[2:9]) for row in grouped[probe]
    ).encode("ascii")).hexdigest() for probe in grouped}


def parse_sums(root):
    result = {}
    for line in (root / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        if len(line) < 67 or line[64:66] != "  ":
            raise ContractError("malformed SHA256SUMS")
        identity, name = line[:64], line[66:]
        if name in result or any(char not in "0123456789abcdef" for char in identity):
            raise ContractError("invalid SHA256SUMS entry")
        result[name] = identity
    return result


def files(root):
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def negative_v1_defect_test(rows, episodes, machine, split):
    broken = [dict(episode) for episode in episodes]
    target = next(episode for episode in broken if int(episode["eligible_rows"]) <
                  int(episode["end_row"]) - int(episode["start_row"]) + 1)
    target["eligible_rows"] = str(int(target["end_row"]) - int(target["start_row"]) + 1)
    try:
        validate_episode_records(rows, broken, machine, split)
    except ContractError as error:
        if "post-exclusion eligibility mismatch" not in str(error):
            raise
        return
    raise ContractError("negative V1 eligibility-defect fixture was accepted")


def validate(root, generator_path):
    manifest = json.loads((root / "manifest.json").read_text(encoding="ascii"))
    if manifest.get("schema") != SCHEMA or manifest.get("rows_per_session") != 320:
        raise ContractError("manifest schema")
    if manifest.get("generator_sha256") != digest(generator_path):
        raise ContractError("generator identity mismatch")
    sums = parse_sums(root)
    expected_inventory = sorted(list(sums) + ["SHA256SUMS"])
    if files(root) != expected_inventory:
        raise ContractError("corpus inventory mismatch")
    for name, expected in sums.items():
        if digest(root / name) != expected:
            raise ContractError(f"checksum mismatch {name}")
    if manifest.get("files") != {name: digest(root / name) for name in manifest["files"]}:
        raise ContractError("manifest file identities")
    all_ids = set()
    outputs = []
    negative_inputs = None
    for record in manifest["splits"]:
        result = validate_split(root, record, all_ids)
        outputs.append((record["machine"], record["split"], result[0], result[1]))
        if record["machine"] == "A" and record["split"] == "calibration":
            negative_inputs = (result[2], result[3])
    if len(manifest["splits"]) != 6 or len(all_ids) != 2 * 40 * 320:
        raise ContractError("split inventory")
    probes = validate_probes(root)
    negative_v1_defect_test(negative_inputs[0], negative_inputs[1], "A", "calibration")
    return outputs, probes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--temp-parent", type=Path)
    args = parser.parse_args()
    root = args.corpus.resolve()
    generator_path = Path(__file__).with_name("generate_r23b_corpus.py").resolve()
    outputs, probes = validate(root, generator_path)
    with tempfile.TemporaryDirectory(prefix="hmi-r23b-verify-", dir=args.temp_parent) as temporary:
        regenerated = Path(temporary) / "corpus"
        generate(regenerated)
        if files(root) != files(regenerated):
            raise SystemExit("FAIL: regenerated inventory differs")
        for name in files(root):
            if digest(root / name) != digest(regenerated / name):
                raise SystemExit(f"FAIL: deterministic regeneration differs: {name}")
    heldout = {machine: counts for machine, split, counts, _ in outputs if split == "heldout"}
    print("R23B_CORPUS_VERIFY "
          f"files={len(files(root))} sha256sums_sha256={digest(root / 'SHA256SUMS')} "
          f"heldout_a={heldout['A']['normal']}N/{heldout['A']['positive']}P "
          f"heldout_b={heldout['B']['normal']}N/{heldout['B']['positive']}P "
          f"probe_a={probes['A_NATIVE'][:16]} probe_b={probes['B_NATIVE'][:16]} "
          "v1_defect_negative=PASS status=PASS")


if __name__ == "__main__":
    try:
        main()
    except (ContractError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        raise SystemExit(f"FAIL: {error}")
