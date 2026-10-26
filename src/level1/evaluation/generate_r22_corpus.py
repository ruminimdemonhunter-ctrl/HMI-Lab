#!/usr/bin/env python3
"""Generate the frozen HMI L1 R2.2 synthetic corpus using integer arithmetic."""

import argparse
import csv
import hashlib
import json
from pathlib import Path

SCHEMA = "HMI_L1_R22_CORPUS_V1"
ROWS_PER_SESSION = 320
SESSION_SPACING_MS = 600_000
SPLITS = (
    ("train", 24, {"A": 1101, "B": 2101}),
    ("calibration", 8, {"A": 1201, "B": 2201}),
    ("heldout", 8, {"A": 1301, "B": 2301}),
)
EVENT_FAMILIES = (
    "cpu_surge", "memory_surge", "io_surge", "overlap",
    "transient_spike", "slow_drift", "regime_change",
)
QUALITY_OK = "OK"
CORPUS_README = """# R2.2 frozen synthetic corpus V1

This directory contains deterministic, nonpersonal synthetic PSI-style
observations. It contains no real host or guest telemetry. Labels and episode
boundaries are engineered research fixtures, not natural-world truth.

Each machine has 40 ordered 320-row sessions: sessions 0--23 are training,
24--31 calibration and 32--39 untouched held-out. Row IDs include machine,
split, session and row and never overlap. Each session declares four normal
and three positive independent scoring windows in a separate episode CSV.
Held-out therefore contains 32 normal and 24 positive windows per machine,
before any R2.3 scoring exclusions, satisfying the preregistered minimums.

The generator uses independent deterministic truth and observation-noise LCG
streams. Seeds are the exact R2.0/R2.2 values. `shared_probe`, `regime_swap`
and `faults` are disjoint from all split row IDs. Fault rows exercise absent,
invalid, stale, regressed, reset, unknown-quality and out-of-range inputs.

`r23_fixed_comparator_v1.json` is frozen now but is not scored in R2.2.
Held-out labels must not be used to revise memory or comparator behavior.

Rebuild and verify every byte with:

    python3 evaluation/verify_r22_corpus.py \\
      --corpus evaluation/r22_corpus_v1

`SHA256SUMS` covers all data, episode, comparator, README and JSON manifest
files. Its own SHA-256 is the aggregate corpus identity printed by the
verification utility.
"""


class Lcg:
    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state

    def bounded(self, limit):
        return self.next() % limit


def clamp(value):
    return max(0, min(10000, value))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def write_ascii(path, text):
    with path.open("w", encoding="ascii", newline="\n") as stream:
        stream.write(text)


def normal_values(machine, row, noise):
    if machine == "A":
        cpu = 850 + (650 if row % 60 < 18 else 0)
        memory = 20 + (row % 11)
        io = 90 + (120 if row % 80 >= 68 else 0)
    else:
        cpu = 350 + (300 if row % 72 < 12 else 0)
        memory = 42 + (28 if row % 90 >= 65 else 0)
        io = 220 + (420 if row % 55 < 15 else 0)
    cpu += int(noise.bounded(101)) - 50
    memory += int(noise.bounded(15)) - 7
    io += int(noise.bounded(81)) - 40
    return clamp(cpu), clamp(memory), clamp(io)


def event_values(family, offset, length, base):
    cpu, memory, io = base
    if family == "cpu_surge":
        cpu = 6200 + (offset % 9) * 90
    elif family == "memory_surge":
        memory = 650 + (offset % 7) * 25
    elif family == "io_surge":
        io = 3100 + (offset % 11) * 80
    elif family == "overlap":
        cpu, memory, io = 5800 + offset % 300, 550 + offset % 90, 2800 + offset % 500
    elif family == "transient_spike":
        center = length // 2
        if center - 3 <= offset <= center + 3:
            cpu, memory, io = 7600, 900, 4200
    elif family == "slow_drift":
        cpu = 1200 + (offset * 5200) // max(1, length - 1)
        memory = 40 + (offset * 620) // max(1, length - 1)
        io = 250 + (offset * 2800) // max(1, length - 1)
    elif family == "regime_change":
        cpu, memory, io = 4200, 320, 1800
    return clamp(cpu), clamp(memory), clamp(io)


def session_episodes(machine, split, session_id, seed):
    truth = Lcg(seed ^ (session_id * 0x9E3779B9) ^ (0xA5A5 if machine == "A" else 0x5A5A))
    starts = (52, 142, 232)
    events = []
    for index, nominal in enumerate(starts):
        start = nominal + int(truth.bounded(9)) - 4
        length = 28 + int(truth.bounded(9))
        family_index = (session_id * 3 + index + int(truth.bounded(len(EVENT_FAMILIES)))) % len(EVENT_FAMILIES)
        events.append({
            "episode_id": f"{machine}-{split}-S{session_id:02d}-P{index}",
            "class": "positive",
            "family": EVENT_FAMILIES[family_index],
            "start_row": start,
            "end_row": start + length - 1,
        })
    normal_ranges = ((0, 39), (92, 129), (182, 219), (276, 319))
    normals = [{
        "episode_id": f"{machine}-{split}-S{session_id:02d}-N{index}",
        "class": "normal",
        "family": "operational_baseline",
        "start_row": start,
        "end_row": end,
    } for index, (start, end) in enumerate(normal_ranges)]
    return normals + events


def fault_for(machine, session_id, row):
    selector = (session_id + (0 if machine == "A" else 3)) % 8
    positions = {
        0: (18, "ABSENT", 1, None),
        1: (96, "INVALID", 0, None),
        2: (188, "STALE", 1, None),
        3: (275, "REGRESSED", 1, "regress"),
        4: (276, "RESET", 1, None),
        5: (22, "UNKNOWN", 1, None),
        6: (97, "OK", 1, "range"),
    }
    return positions.get(selector) if selector in positions and positions[selector][0] == row else None


def write_split(output, machine, split, count, seed, first_session):
    data_name = f"machine_{machine.lower()}_{split}.csv"
    episode_name = f"machine_{machine.lower()}_{split}_episodes.csv"
    data_path = output / data_name
    episode_path = output / episode_name
    fieldnames = (
        "schema", "row_id", "machine", "split", "session_id", "row_in_session",
        "monotonic_ms", "valid", "quality", "cpu_some_bp", "memory_full_bp",
        "io_full_bp", "scoring_class", "event_family", "episode_id", "exclude_reason",
    )
    episode_fields = (
        "schema", "machine", "split", "session_id", "episode_id", "scoring_class",
        "event_family", "start_row", "end_row", "eligible_rows",
    )
    rows = 0
    excluded = 0
    positive_windows = 0
    normal_windows = 0
    with data_path.open("w", newline="", encoding="ascii") as data_stream, \
            episode_path.open("w", newline="", encoding="ascii") as episode_stream:
        writer = csv.DictWriter(data_stream, fieldnames=fieldnames, lineterminator="\n")
        episode_writer = csv.DictWriter(episode_stream, fieldnames=episode_fields, lineterminator="\n")
        writer.writeheader()
        episode_writer.writeheader()
        for local_session in range(count):
            session_id = first_session + local_session
            episodes = session_episodes(machine, split, session_id, seed)
            episode_by_row = {}
            for episode in episodes:
                eligible = episode["end_row"] - episode["start_row"] + 1
                episode_writer.writerow({
                    "schema": SCHEMA,
                    "machine": machine,
                    "split": split,
                    "session_id": session_id,
                    "episode_id": episode["episode_id"],
                    "scoring_class": episode["class"],
                    "event_family": episode["family"],
                    "start_row": episode["start_row"],
                    "end_row": episode["end_row"],
                    "eligible_rows": eligible,
                })
                if episode["class"] == "positive":
                    positive_windows += 1
                else:
                    normal_windows += 1
                for row in range(episode["start_row"], episode["end_row"] + 1):
                    episode_by_row[row] = episode
            noise = Lcg((seed << 16) ^ session_id ^ (0x13579BDF if machine == "A" else 0x2468ACE0))
            base_time = session_id * SESSION_SPACING_MS
            previous_time = base_time
            for row in range(ROWS_PER_SESSION):
                cpu, memory, io = normal_values(machine, row, noise)
                episode = episode_by_row.get(row)
                if episode and episode["class"] == "positive":
                    cpu, memory, io = event_values(
                        episode["family"], row - episode["start_row"],
                        episode["end_row"] - episode["start_row"] + 1,
                        (cpu, memory, io))
                quality = QUALITY_OK
                valid = 1
                exclude_reason = ""
                timestamp = base_time + row * 1000
                fault = fault_for(machine, session_id, row)
                if fault:
                    _, quality, valid, mutation = fault
                    exclude_reason = "invalid_input"
                    if mutation == "regress":
                        timestamp = previous_time - 500
                    elif mutation == "range":
                        cpu = 10001
                if timestamp >= previous_time:
                    previous_time = timestamp
                scoring_class = episode["class"] if episode else "context"
                family = episode["family"] if episode else "unscored_transition"
                episode_id = episode["episode_id"] if episode else ""
                if exclude_reason:
                    excluded += 1
                writer.writerow({
                    "schema": SCHEMA,
                    "row_id": f"{machine}-{split}-S{session_id:02d}-R{row:03d}",
                    "machine": machine,
                    "split": split,
                    "session_id": session_id,
                    "row_in_session": row,
                    "monotonic_ms": timestamp,
                    "valid": valid,
                    "quality": quality,
                    "cpu_some_bp": cpu,
                    "memory_full_bp": memory,
                    "io_full_bp": io,
                    "scoring_class": scoring_class,
                    "event_family": family,
                    "episode_id": episode_id,
                    "exclude_reason": exclude_reason,
                })
                rows += 1
    return {
        "machine": machine,
        "split": split,
        "seed": seed,
        "sessions": count,
        "rows": rows,
        "positive_windows": positive_windows,
        "normal_windows": normal_windows,
        "excluded_rows": excluded,
        "data_file": data_name,
        "episode_file": episode_name,
    }


def write_probe(output, name, seed, machine_mode, fault_only=False):
    path = output / f"{name}.csv"
    fields = (
        "schema", "row_id", "probe", "row", "monotonic_ms", "valid", "quality",
        "cpu_some_bp", "memory_full_bp", "io_full_bp", "truth_label", "episode_id",
    )
    truth = Lcg(seed)
    noise = Lcg(seed ^ 0xD00DFEED)
    probe_families = [EVENT_FAMILIES[truth.bounded(len(EVENT_FAMILIES))] for _ in range(4)]
    with path.open("w", newline="", encoding="ascii") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for row in range(640):
            machine = machine_mode if machine_mode in ("A", "B") else ("A" if row < 320 else "B")
            if name == "regime_swap":
                first_half = row % 320 < 160
                machine = ("A" if first_half else "B") if row < 320 else ("B" if first_half else "A")
            cpu, memory, io = normal_values(machine, row % 320, noise)
            valid = 1
            quality = QUALITY_OK
            label = "normal"
            episode_id = ""
            if fault_only:
                fault_types = ("ABSENT", "INVALID", "STALE", "REGRESSED", "RESET", "UNKNOWN")
                quality = fault_types[(row // 20) % len(fault_types)]
                valid = 0 if quality == "INVALID" else 1
                label = "excluded_fault"
                episode_id = f"{name}-F{row // 20:02d}"
                if row % 53 == 0:
                    cpu = 10001
            elif 120 <= row % 320 < 160 or 240 <= row % 320 < 280:
                start = 120 if row % 320 < 160 else 240
                family = probe_families[(row // 320) * 2 + (0 if start == 120 else 1)]
                cpu, memory, io = event_values(family, row % 320 - start, 40, (cpu, memory, io))
                label = "positive"
                episode_id = f"{name}-P{row // 320}-{start}"
            writer.writerow({
                "schema": SCHEMA,
                "row_id": f"{name}-R{row:03d}",
                "probe": name,
                "row": row,
                "monotonic_ms": row * 1000,
                "valid": valid,
                "quality": quality,
                "cpu_some_bp": cpu,
                "memory_full_bp": memory,
                "io_full_bp": io,
                "truth_label": label,
                "episode_id": episode_id,
            })
    return {"name": name, "seed": seed, "rows": 640, "file": path.name}


def write_comparator(output):
    comparator = {
        "schema": "HMI_L1_R23_FIXED_COMPARATOR_V1",
        "status": "frozen_before_r2.2_memory_implementation",
        "inputs": "eligible quality=OK observations only",
        "thresholds_bp": {"cpu_some": 6000, "memory_full": 400, "io_full": 2000},
        "row_rule": "positive if any threshold is met; otherwise normal",
        "window_rule": "positive if at least 5 eligible rows are positive; abstain if fewer than 10 eligible rows",
        "tuning": "none; values fixed before learner implementation and held-out scoring",
        "scientific_use": "R2.3 only; not scored or revised in R2.2",
    }
    path = output / "r23_fixed_comparator_v1.json"
    write_ascii(path, json.dumps(comparator, indent=2, sort_keys=True) + "\n")
    return path.name


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    write_ascii(output / "README.md", CORPUS_README)
    split_records = []
    first_session = 0
    for split, count, seeds in SPLITS:
        for machine in ("A", "B"):
            split_records.append(write_split(output, machine, split, count, seeds[machine], first_session))
        first_session += count
    probes = [
        write_probe(output, "shared_probe", 9001, "AB"),
        write_probe(output, "regime_swap", 9002, "AB"),
        write_probe(output, "faults", 9003, "A", fault_only=True),
    ]
    comparator = write_comparator(output)
    content_files = sorted(
        path.name for path in output.iterdir()
        if path.is_file() and path.name not in ("manifest.json", "SHA256SUMS")
    )
    identities = {name: sha256(output / name) for name in content_files}
    manifest = {
        "schema": SCHEMA,
        "generator": "generate_r22_corpus.py",
        "rows_per_session": ROWS_PER_SESSION,
        "split_order": ["train", "calibration", "heldout"],
        "splits": split_records,
        "probes": probes,
        "comparator_file": comparator,
        "files": identities,
        "heldout_support_gate": {
            "minimum_positive_windows_per_machine": 20,
            "minimum_normal_windows_per_machine": 20,
            "status": "PASS",
        },
        "claims": "synthetic engineered labels only; R2.2 does not score utility or individuality",
    }
    manifest_path = output / "manifest.json"
    write_ascii(manifest_path, json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    checksum_names = sorted(content_files + [manifest_path.name])
    checksum_text = "".join(f"{sha256(output / name)}  {name}\n" for name in checksum_names)
    write_ascii(output / "SHA256SUMS", checksum_text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.output)


if __name__ == "__main__":
    main()
