#!/usr/bin/env python3
"""Generate the frozen, synthetic-only HMI L1 R2.3b corpus V2."""

import argparse
import csv
import hashlib
import json
from pathlib import Path

SCHEMA = "HMI_L1_R23B_CORPUS_V2"
ROWS_PER_SESSION = 320
SESSION_SPACING_MS = 1_000_000
SPLITS = (
    ("train", 24, {"A": 3101, "B": 4101}, 0),
    ("calibration", 8, {"A": 3201, "B": 4201}, 24),
    ("heldout", 8, {"A": 3301, "B": 4301}, 32),
)
POSITIVE_FAMILIES = (
    "high_departure", "low_departure", "moderate_departure",
    "transition", "temporal_reorder",
)
QUALITIES = ("OK", "ABSENT", "INVALID", "STALE", "REGRESSED", "RESET")


class Lcg:
    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state

    def bounded(self, limit):
        return self.next() % limit

    def signed(self, magnitude):
        return self.bounded(2 * magnitude + 1) - magnitude


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_ascii(path, text):
    path.write_text(text, encoding="ascii", newline="\n")


def baseline(machine, row, noise):
    if machine == "A":
        cpu = 3900 + ((row * 37) % 601) - 300 + noise.signed(90)
        memory = 58 + ((row * 11) % 31) - 15 + noise.signed(4)
        io = 170 + ((row * 17) % 81) - 40 + noise.signed(10)
    else:
        cpu = 625 + ((row * 29) % 351) - 175 + noise.signed(70)
        memory = 190 + ((row * 13) % 91) - 45 + noise.signed(8)
        io = 1400 + ((row * 19) % 601) - 300 + noise.signed(70)
    return tuple(max(0, min(10000, value)) for value in (cpu, memory, io))


def departure(machine, family, offset, length, current):
    cpu, memory, io = current
    other = "B" if machine == "A" else "A"
    if family == "cross_profile":
        fixed = Lcg((offset + 1) * (0x34567 if other == "A" else 0x789AB))
        return baseline(other, offset, fixed)
    if family == "high_departure":
        return (7600 + offset * 23 % 900, 720 + offset * 17 % 260,
                3300 + offset * 31 % 900)
    if family == "low_departure":
        return (20 + offset % 15, offset % 3, 5 + offset % 9)
    if family == "moderate_departure":
        if machine == "A":
            return (2100 + offset * 19 % 300, 170 + offset % 25,
                    760 + offset * 13 % 180)
        return (1900 + offset * 17 % 350, 55 + offset % 18,
                360 + offset * 11 % 100)
    if family == "transition":
        if offset < length // 2:
            return current
        return departure(machine, "cross_profile", offset, length, current)
    if family == "temporal_reorder":
        # Same marginal bins as ordinary baseline, but deliberately blocked order.
        if machine == "A":
            return (3500, 42, 135) if offset < length // 2 else (4400, 74, 220)
        return (450, 145, 1120) if offset < length // 2 else (820, 245, 1720)
    raise ValueError(f"unknown family {family}")


def episodes_for(machine, split, session, seed):
    truth = Lcg((seed << 12) ^ (session * 0x9E3779B9) ^ ord(machine))
    normal_starts = (8, 48, 88, 128)
    positive_starts = (188, 233, 278)
    episodes = []
    for index, start in enumerate(normal_starts):
        jitter = truth.bounded(5)
        episodes.append({
            "id": f"{machine}-{split}-S{session:02d}-N{index}",
            "class": "normal", "family": "operational_baseline",
            "start": start + jitter, "end": start + jitter + 19,
        })
    rotation = truth.bounded(len(POSITIVE_FAMILIES))
    for index, start in enumerate(positive_starts):
        jitter = truth.bounded(5)
        family = "cross_profile" if index == 0 else \
            POSITIVE_FAMILIES[(rotation + session + index) % len(POSITIVE_FAMILIES)]
        episodes.append({
            "id": f"{machine}-{split}-S{session:02d}-P{index}",
            "class": "positive", "family": family,
            "start": start + jitter, "end": start + jitter + 19,
        })
    return episodes


def faults_for(machine, split, session, seed):
    stream = Lcg((seed << 7) ^ (9301 << 3) ^ (session * 0x45D9F3B) ^
                 (0xA5 if machine == "A" else 0x5A))
    kinds = ("ABSENT", "INVALID", "STALE", "REGRESSED", "RESET",
             "UNKNOWN", "FALSE_VALID", "RANGE", "TIME_REGRESSION")
    result = {}
    for index in range(2):
        row = 12 + stream.bounded(285)
        while row in result:
            row = 12 + stream.bounded(285)
        kind = kinds[(session * 2 + index + (0 if machine == "A" else 3)) % len(kinds)]
        if split == "train" and session == 23 and kind in \
                ("REGRESSED", "RESET", "TIME_REGRESSION"):
            kind = "INVALID" if index == 0 else "STALE"
        result[row] = kind
    return result


def apply_fault(kind, timestamp, values):
    quality, valid = "OK", 1
    cpu, memory, io = values
    if kind in QUALITIES[1:]:
        quality = kind
    elif kind == "UNKNOWN":
        quality = "UNKNOWN"
    elif kind == "FALSE_VALID":
        valid = 0
    elif kind == "RANGE":
        cpu = 10001
    elif kind == "TIME_REGRESSION":
        timestamp -= 1500
    return timestamp, valid, quality, (cpu, memory, io), kind.lower()


def row_eligible(row, previous_timestamp):
    numeric = all(0 <= int(row[name]) <= 10000 for name in
                  ("cpu_some_bp", "memory_full_bp", "io_full_bp"))
    monotonic = previous_timestamp is None or int(row["monotonic_ms"]) > previous_timestamp
    return (row["quality"] == "OK" and row["valid"] == "1" and numeric and
            monotonic and row["exclude_reason"] == "")


def write_split(output, machine, split, sessions, seed, first_session):
    data_name = f"machine_{machine.lower()}_{split}.csv"
    episode_name = f"machine_{machine.lower()}_{split}_episodes.csv"
    row_fields = ("schema", "row_id", "machine", "split", "session_id",
                  "row_in_session", "monotonic_ms", "valid", "quality",
                  "cpu_some_bp", "memory_full_bp", "io_full_bp",
                  "scoring_class", "event_family", "episode_id", "exclude_reason")
    episode_fields = ("schema", "machine", "split", "session_id", "episode_id",
                      "scoring_class", "event_family", "start_row", "end_row",
                      "eligible_rows")
    all_rows, all_episodes = [], []
    for local in range(sessions):
        session = first_session + local
        episodes = episodes_for(machine, split, session, seed)
        by_row = {}
        for episode in episodes:
            for row in range(episode["start"], episode["end"] + 1):
                by_row[row] = episode
        faults = faults_for(machine, split, session, seed)
        noise = Lcg((seed << 16) ^ session ^ (0x13579BDF if machine == "A" else 0x2468ACE0))
        base_time = session * SESSION_SPACING_MS
        previous_raw = None
        session_rows = []
        for row_index in range(ROWS_PER_SESSION):
            values = baseline(machine, row_index, noise)
            episode = by_row.get(row_index)
            if episode is not None and episode["class"] == "positive":
                values = departure(machine, episode["family"],
                                   row_index - episode["start"], 20, values)
            timestamp = base_time + row_index * 1000
            valid, quality, exclude = 1, "OK", ""
            if row_index in faults:
                timestamp, valid, quality, values, exclude = apply_fault(
                    faults[row_index], timestamp, values)
            record = {
                "schema": SCHEMA,
                "row_id": f"{machine}-{split}-S{session:02d}-R{row_index:03d}",
                "machine": machine, "split": split, "session_id": str(session),
                "row_in_session": str(row_index), "monotonic_ms": str(timestamp),
                "valid": str(valid), "quality": quality,
                "cpu_some_bp": str(values[0]), "memory_full_bp": str(values[1]),
                "io_full_bp": str(values[2]),
                "scoring_class": episode["class"] if episode else "context",
                "event_family": episode["family"] if episode else "unscored_transition",
                "episode_id": episode["id"] if episode else "",
                "exclude_reason": exclude,
            }
            # A timestamp regression is ineligible even if its explicit reason were lost.
            if previous_raw is not None and timestamp <= previous_raw and not exclude:
                record["exclude_reason"] = "time_regression"
            previous_raw = max(previous_raw, timestamp) if previous_raw is not None else timestamp
            session_rows.append(record)
        for episode in episodes:
            previous = None
            eligible = 0
            for record in session_rows:
                if int(record["row_in_session"]) < episode["start"] or \
                        int(record["row_in_session"]) > episode["end"]:
                    continue
                if row_eligible(record, previous):
                    eligible += 1
                    previous = int(record["monotonic_ms"])
            all_episodes.append({
                "schema": SCHEMA, "machine": machine, "split": split,
                "session_id": str(session), "episode_id": episode["id"],
                "scoring_class": episode["class"], "event_family": episode["family"],
                "start_row": str(episode["start"]), "end_row": str(episode["end"]),
                "eligible_rows": str(eligible),
            })
        all_rows.extend(session_rows)
    with (output / data_name).open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=row_fields, lineterminator="\n")
        writer.writeheader(); writer.writerows(all_rows)
    with (output / episode_name).open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=episode_fields, lineterminator="\n")
        writer.writeheader(); writer.writerows(all_episodes)
    scoreable = [episode for episode in all_episodes if int(episode["eligible_rows"]) >= 10]
    return {
        "machine": machine, "split": split, "seed": seed, "sessions": sessions,
        "rows": len(all_rows), "data_file": data_name, "episode_file": episode_name,
        "normal_episodes": sum(e["scoring_class"] == "normal" for e in scoreable),
        "positive_episodes": sum(e["scoring_class"] == "positive" for e in scoreable),
        "excluded_rows": sum(not row_eligible(row, None) for row in all_rows),
    }


def write_probes(output):
    row_fields = ("schema", "probe_id", "row", "monotonic_ms", "valid", "quality",
                  "cpu_some_bp", "memory_full_bp", "io_full_bp", "exclude_reason")
    truth_fields = ("schema", "probe_id", "machine", "truth_label", "truth_origin")
    rows, truths = [], []
    for probe, mode, seed in (("A_NATIVE", "A", 9101), ("B_NATIVE", "B", 9102)):
        noise = Lcg(seed)
        for index in range(40):
            values = baseline(mode, index, noise)
            rows.append({"schema": SCHEMA, "probe_id": probe, "row": index,
                         "monotonic_ms": index * 1000, "valid": 1, "quality": "OK",
                         "cpu_some_bp": values[0], "memory_full_bp": values[1],
                         "io_full_bp": values[2], "exclude_reason": ""})
        truths.extend((
            {"schema": SCHEMA, "probe_id": probe, "machine": "A",
             "truth_label": "EXPECTED" if mode == "A" else "SUSPECTED",
             "truth_origin": f"generated_from_{mode}_latent_profile"},
            {"schema": SCHEMA, "probe_id": probe, "machine": "B",
             "truth_label": "EXPECTED" if mode == "B" else "SUSPECTED",
             "truth_origin": f"generated_from_{mode}_latent_profile"},
        ))
    for index in range(40):
        rows.append({"schema": SCHEMA, "probe_id": "INVALID_IDENTICAL", "row": index,
                     "monotonic_ms": index * 1000, "valid": 0, "quality": "INVALID",
                     "cpu_some_bp": 0, "memory_full_bp": 0, "io_full_bp": 0,
                     "exclude_reason": "invalid_probe"})
    for machine in ("A", "B"):
        truths.append({"schema": SCHEMA, "probe_id": "INVALID_IDENTICAL",
                       "machine": machine, "truth_label": "UNKNOWN",
                       "truth_origin": "predeclared_invalid_quality"})
    with (output / "contextual_probes.csv").open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=row_fields, lineterminator="\n")
        writer.writeheader(); writer.writerows(rows)
    with (output / "contextual_probe_truth.csv").open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=truth_fields, lineterminator="\n")
        writer.writeheader(); writer.writerows(truths)
    # A fixed training-style regime swap diagnostic, never used for calibration selection.
    with (output / "regime_shift.csv").open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=row_fields, lineterminator="\n")
        writer.writeheader()
        noise = Lcg(9201)
        for index in range(640):
            mode = "A" if index < 320 else "B"
            values = baseline(mode, index % 320, noise)
            writer.writerow({"schema": SCHEMA, "probe_id": "REGIME_SHIFT_A_TO_B",
                             "row": index, "monotonic_ms": index * 1000,
                             "valid": 1, "quality": "OK", "cpu_some_bp": values[0],
                             "memory_full_bp": values[1], "io_full_bp": values[2],
                             "exclude_reason": ""})
    # Explicit faults table is separate from split rows and not scored as utility evidence.
    with (output / "fault_probes.csv").open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=row_fields, lineterminator="\n")
        writer.writeheader()
        fault_noise = Lcg(9301)
        for index, kind in enumerate(("ABSENT", "INVALID", "STALE", "REGRESSED",
                                      "RESET", "UNKNOWN", "FALSE_VALID", "RANGE",
                                      "TIME_REGRESSION")):
            base_values = (100 + fault_noise.bounded(50), 10 + fault_noise.bounded(10),
                           20 + fault_noise.bounded(20))
            timestamp, valid, quality, values, reason = apply_fault(
                kind, index * 1000, base_values)
            writer.writerow({"schema": SCHEMA, "probe_id": "FAULTS", "row": index,
                             "monotonic_ms": timestamp, "valid": valid, "quality": quality,
                             "cpu_some_bp": values[0], "memory_full_bp": values[1],
                             "io_full_bp": values[2], "exclude_reason": reason})


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    write_ascii(output / "README.md",
                "# HMI L1 R2.3b synthetic corpus V2\n\n"
                "Fresh deterministic synthetic machine-condition observations only. "
                "Labels are engineered relative to latent A/B profiles and are not real-device truth.\n")
    split_records = []
    for split, sessions, seeds, first in SPLITS:
        for machine in ("A", "B"):
            split_records.append(write_split(output, machine, split, sessions,
                                             seeds[machine], first))
    write_probes(output)
    comparator = {
        "schema": "HMI_L1_R23B_FIXED_REFERENCES_V1",
        "v1_original": {"cpu": 6000, "memory": 400, "io": 2000, "k": 5},
        "high_side_grid": {"cpu": [1000, 2000, 3000, 4500, 6000, 8000],
                           "memory": [25, 50, 100, 200, 400, 800],
                           "io": [250, 500, 1000, 2000, 3000, 5000],
                           "k": [1, 3, 5, 8, 12]},
        "two_sided_q_percent": [0, 1, 2, 5, 10, 20],
        "two_sided_k": [1, 3, 5, 8, 12],
    }
    write_ascii(output / "fixed_references_v1.json",
                json.dumps(comparator, indent=2, sort_keys=True) + "\n")
    content = sorted(path.name for path in output.iterdir()
                     if path.is_file() and path.name not in ("manifest.json", "SHA256SUMS"))
    identities = {name: sha256(output / name) for name in content}
    generator_path = Path(__file__).resolve()
    manifest = {
        "schema": SCHEMA, "generator": generator_path.name,
        "generator_sha256": sha256(generator_path), "rows_per_session": ROWS_PER_SESSION,
        "split_order": ["train", "calibration", "heldout"], "splits": split_records,
        "seeds": {"A": {"train": 3101, "calibration": 3201, "heldout": 3301},
                  "B": {"train": 4101, "calibration": 4201, "heldout": 4301},
                  "A_NATIVE": 9101, "B_NATIVE": 9102, "regime_shift": 9201,
                  "faults": 9301},
        "probe_files": ["contextual_probes.csv", "contextual_probe_truth.csv",
                        "regime_shift.csv", "fault_probes.csv"],
        "files": identities,
        "claims": "synthetic engineered contextual truth only; no real workload evidence",
    }
    write_ascii(output / "manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    checksum_names = sorted(content + ["manifest.json"])
    write_ascii(output / "SHA256SUMS", "".join(
        f"{sha256(output / name)}  {name}\n" for name in checksum_names))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.output)


if __name__ == "__main__":
    main()
