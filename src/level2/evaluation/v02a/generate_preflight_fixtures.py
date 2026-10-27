#!/usr/bin/env python3
"""Generate the tiny deterministic L2 V0.2a preflight corpus.

This is deliberately not a scientific study corpus.  It contains only
known-answer controls for the offline evaluator.
"""

from __future__ import annotations

import csv
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "fixtures"
EVENT_HEADER = (
    "schema,seq,monotonic_ms,quality,event,workspace,role,activity,"
    "l1_condition,provenance\n"
)
A = ("CODE_EDIT", "BUILD_TEST", "DOC_VIEW")
B = ("REFERENCES", "CANVAS", "ASSET_GROUP")
C = ("DOC_VIEW", "REFERENCES", "CANVAS")


def event(seq: int, quality: str, kind: str, workspace: int, role: str = "NONE",
          activity: str = "NONE", condition: str = "NONE",
          provenance: str = "SIMULATED") -> str:
    return (
        f"HMI_L2_EVENT_V1,{seq},{seq * 1000},{quality},{kind},{workspace},"
        f"{role},{activity},{condition},{provenance}\n"
    )


def history(sessions: list[tuple[str, str, str]], *, workspace: int = 1,
            reset_after: int | None = None, pressure: bool = False,
            invalid_before_query: bool = False, provenance: str = "SIMULATED") -> str:
    rows: list[str] = []
    seq = 1
    rows.append(event(seq, "OK", "CONTEXT", workspace, activity="INTERACTIVE",
                      condition="QUIET", provenance=provenance))
    seq += 1
    for index, triple in enumerate(sessions):
        if reset_after is not None and index == reset_after:
            rows.append(event(seq, "RESET", "RESET", 0, provenance=provenance))
            seq += 1
            rows.append(event(seq, "OK", "CONTEXT", workspace,
                              activity="INTERACTIVE", condition="QUIET",
                              provenance=provenance))
            seq += 1
        rows.append(event(seq, "OK", "SESSION_BEGIN", workspace,
                          provenance=provenance))
        seq += 1
        for role in triple:
            rows.append(event(seq, "OK", "ROLE", workspace, role=role,
                              provenance=provenance))
            seq += 1
        rows.append(event(seq, "OK", "SESSION_END", workspace,
                          provenance=provenance))
        seq += 1
    quality = "INVALID" if invalid_before_query else "OK"
    condition = "MEMORY_PRESSURE" if pressure else "QUIET"
    rows.append(event(seq, quality, "CONTEXT", workspace, activity="INTERACTIVE",
                      condition=condition, provenance=provenance))
    seq += 1
    rows.append(event(seq, "OK", "QUERY", workspace, provenance=provenance))
    return EVENT_HEADER + "".join(rows)


EPISODES = [
    # Core support, truth separation, twins, cold/reset/safety controls.
    ("supported_a", "A", 1, [A, A, A], None, False, False, A, "DETERMINATE", "ALLOW", "core"),
    ("supported_b", "B", 2, [B, B, B], None, False, False, B, "DETERMINATE", "ALLOW", "core"),
    ("twin_left", "T", 1, [A, A, A], None, False, False, A, "DETERMINATE", "ALLOW", "twins"),
    ("twin_right", "T", 1, [A, A, A], None, False, False, B, "DETERMINATE", "ALLOW", "twins"),
    ("cold", "C", 1, [A], None, False, False, A, "DETERMINATE", "ALLOW", "support"),
    ("late_reset", "R", 1, [A, A, A, A, A], 3, False, False, A, "DETERMINATE", "ALLOW", "support"),
    ("resource_pressure", "P", 1, [A, A, A], None, True, False, A, "DETERMINATE", "RESOURCE_PRESSURE", "safety"),
    ("invalid_source", "I", 1, [A, A, A], None, False, True, A, "DETERMINATE", "INVALID_SOURCE", "safety"),
    ("deny", "D", 1, [A, A, A], None, False, False, A, "DETERMINATE", "DENY", "safety"),
    # Recent-N known-answer suite: final B is correct while lifetime V0.1 stays A.
    ("recent_1", "CR", 1, [A, A, A, B], None, False, False, B, "DETERMINATE", "ALLOW", "cal_recent"),
    ("recent_2", "CR", 1, [A, A, A, B], None, False, False, B, "DETERMINATE", "ALLOW", "cal_recent"),
    ("recent_3", "CR", 1, [A, A, A, B], None, False, False, B, "DETERMINATE", "ALLOW", "cal_recent"),
    # Static-workspace known answer: recent C is wrong, training template A is right.
    ("static_1", "CS", 1, [A, A, A, C], None, False, False, A, "DETERMINATE", "ALLOW", "cal_static"),
    ("static_2", "CS", 1, [A, A, A, C], None, False, False, A, "DETERMINATE", "ALLOW", "cal_static"),
    ("static_3", "CS", 1, [A, A, A, C], None, False, False, A, "DETERMINATE", "ALLOW", "cal_static"),
    # NONE known answer: ambiguity makes every candidate worse than abstention.
    ("none_1", "CN", 1, [A, A, A], None, False, False, A, "AMBIGUOUS", "ALLOW", "cal_none"),
    ("none_2", "CN", 1, [A, A, A], None, False, False, A, "AMBIGUOUS", "ALLOW", "cal_none"),
    ("none_3", "CN", 1, [A, A, A], None, False, False, A, "AMBIGUOUS", "ALLOW", "cal_none"),
]


def support_after_reset(sessions: list[tuple[str, str, str]], reset_after: int | None):
    kept = sessions[reset_after:] if reset_after is not None else sessions
    counts: dict[tuple[str, str, str], int] = {}
    for triple in kept:
        counts[triple] = counts.get(triple, 0) + 1
    return len(kept), len(kept) * 2, max(counts.values(), default=0)


def write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    with path.open("w", encoding="ascii", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    metadata = []
    truth = []
    for (episode, device, workspace, sessions, reset_after, pressure,
         invalid, expected, quality, guard, suite) in EPISODES:
        text = history(sessions, workspace=workspace, reset_after=reset_after,
                       pressure=pressure, invalid_before_query=invalid)
        filename = f"history_{episode}.csv"
        (OUT / filename).write_text(text, encoding="ascii", newline="")
        completed, transitions, triple_count = support_after_reset(sessions, reset_after)
        metadata.append([episode, filename, workspace, len(text.splitlines()) - 1,
                         completed, transitions, triple_count, suite])
        truth.append([episode, device, workspace, suite,
                      len(text.splitlines()) - 1, *expected, quality, guard])

    write_csv(OUT / "metadata.csv",
              ["episode_id", "history_file", "workspace_id", "allowed_query_index",
               "declared_sessions", "declared_transitions", "declared_triple_count", "suite"],
              metadata)
    write_csv(OUT / "truth.csv",
              ["episode_id", "device_id", "workspace_id", "regime", "allowed_query_index",
               "truth_r1", "truth_r2", "truth_r3", "truth_quality", "safety_guard"],
              truth)
    write_csv(OUT / "training.csv",
              ["training_id", "workspace_id", "r1", "r2", "r3"],
              [["train_a1", 1, *A], ["train_a2", 1, *A], ["train_a3", 1, *A],
               ["train_b1", 2, *B], ["train_b2", 2, *B]])

    # Committed invalid controls. They must never be accepted as corpus members.
    (OUT / "negative_bad_provenance.csv").write_text(
        history([A, A, A], provenance="OBSERVED"), encoding="ascii", newline="")
    write_csv(OUT / "negative_false_support.csv",
              ["episode_id", "history_file", "workspace_id", "allowed_query_index",
               "declared_sessions", "declared_transitions", "declared_triple_count", "suite"],
              [["cold", "history_cold.csv", 1, 8, 99, 99, 99, "support"]])
    write_csv(OUT / "negative_truth_mismatch.csv",
              ["episode_id", "device_id", "workspace_id", "regime", "allowed_query_index",
               "truth_r1", "truth_r2", "truth_r3", "truth_quality", "safety_guard"],
              [["unknown_episode", "X", 1, "core", 20, *A, "DETERMINATE", "ALLOW"]])
    write_csv(OUT / "negative_duplicate_truth.csv",
              ["episode_id", "device_id", "workspace_id", "regime", "allowed_query_index",
               "truth_r1", "truth_r2", "truth_r3", "truth_quality", "safety_guard"],
              [["supported_a", "A", 1, "core", 20, *A, "DETERMINATE", "ALLOW"],
               ["supported_a", "A", 1, "core", 20, *A, "DETERMINATE", "ALLOW"]])
    (OUT / "negative_truth_leak.csv").write_text(
        history([A, A, A]).replace("SIMULATED\n", "SIMULATED,TRUTH_CODE_EDIT\n", 1),
        encoding="ascii", newline="")

    names = sorted(path.name for path in OUT.iterdir() if path.name != "SHA256SUMS")
    with (OUT / "SHA256SUMS").open("w", encoding="ascii", newline="") as manifest:
        for name in names:
            digest = hashlib.sha256((OUT / name).read_bytes()).hexdigest()
            manifest.write(f"{digest}  {name}\n")
    print(f"V02A_FIXTURE_GENERATE files={len(names)} episodes={len(EPISODES)} study=NONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
