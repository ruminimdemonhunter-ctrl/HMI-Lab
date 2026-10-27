#!/usr/bin/env python3
"""Executable, offline-only V0.2a comparators over identical permitted history."""

from __future__ import annotations

import csv
import re
import subprocess
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from verify_preflight_fixtures import (CONTEXT_HEADER, FIX, META_HEADER, ROLES,
                                       inspect_history, strict_rows)

GRID = (1, 2, 3, 5, 8)
PREDICTION_HEADER = ["episode_id", "candidate_id", "status", "r1", "r2", "r3", "reason"]
ALLOWED_CANDIDATES = tuple([f"RECENT_{n}" for n in GRID] +
                           ["STATIC_WORKSPACE", "STATIC_GLOBAL", "ALWAYS_NONE", "V01_FROZEN"])


@dataclass(frozen=True)
class Prediction:
    episode_id: str
    candidate_id: str
    status: str
    roles: tuple[str, str, str]
    reason: str

    def row(self) -> list[str]:
        return [self.episode_id, self.candidate_id, self.status, *self.roles, self.reason]


def none(episode: str, candidate: str, reason: str) -> Prediction:
    return Prediction(episode, candidate, "NONE", ("NONE", "NONE", "NONE"), reason)


def blocked(episode: str, candidate: str, reason: str) -> Prediction:
    return Prediction(episode, candidate, "BLOCKED", ("NONE", "NONE", "NONE"), reason)


def guard_prediction(episode: str, candidate: str, context: dict[str, str]) -> Prediction | None:
    if context["source_quality"] == "INVALID" or context["freshness"] == "STALE":
        return none(episode, candidate, "INVALID_SOURCE")
    if context["permission"] == "DENY":
        return blocked(episode, candidate, "DENIED")
    if context["resource_state"] == "PRESSURE":
        return blocked(episode, candidate, "RESOURCE_CONSTRAINED")
    return None


def best(triples: list[tuple[str, str, str]]) -> tuple[str, str, str]:
    counts = Counter(triples)
    if not counts:
        raise ValueError("no triples")
    return min(counts, key=lambda triple: (-counts[triple], triple))


def training_templates() -> tuple[dict[int, tuple[str, str, str]], tuple[str, str, str]]:
    header = ["training_id", "workspace_id", "r1", "r2", "r3"]
    rows = strict_rows(FIX / "training.csv", header, 64)
    per_workspace: dict[int, list[tuple[str, str, str]]] = {}
    all_triples = []
    seen = set()
    for row in rows:
        if row["training_id"] in seen:
            raise ValueError("duplicate training ID")
        seen.add(row["training_id"])
        workspace = int(row["workspace_id"])
        triple = (row["r1"], row["r2"], row["r3"])
        if workspace not in range(1, 5) or any(role not in ROLES for role in triple):
            raise ValueError("bad training row")
        per_workspace.setdefault(workspace, []).append(triple)
        all_triples.append(triple)
    return {workspace: best(triples) for workspace, triples in per_workspace.items()}, best(all_triples)


def run_baseline(candidate: str, episode: str, state: dict[str, object],
                 context: dict[str, str]) -> Prediction:
    if candidate not in ALLOWED_CANDIDATES or candidate == "V01_FROZEN":
        raise ValueError("candidate")
    guarded = guard_prediction(episode, candidate, context)
    if guarded:
        return guarded
    if state["sessions"] < 3 or state["transitions"] < 6 or state["triple_count"] < 2:
        return none(episode, candidate, "WARMING_UP")
    if candidate == "ALWAYS_NONE":
        return none(episode, candidate, "SAFE_ABSTAIN")
    workspace_templates, global_template = training_templates()
    if candidate == "STATIC_WORKSPACE":
        triple = workspace_templates[int(state["workspace"])]
    elif candidate == "STATIC_GLOBAL":
        triple = global_template
    else:
        if not candidate.startswith("RECENT_"):
            raise ValueError("candidate")
        n = int(candidate.removeprefix("RECENT_"))
        if n not in GRID:
            raise ValueError("recent grid")
        recent_sessions = list(state["session_roles"])[-n:]
        triples = [roles[index:index + 3] for roles in recent_sessions
                   for index in range(len(roles) - 2)]
        triple = best(triples)
    return Prediction(episode, candidate, "CANDIDATE", triple, "SUPPORTED_HISTORY")


def run_v01(episode: str, history: Path, context: dict[str, str], executable: Path) -> Prediction:
    candidate = "V01_FROZEN"
    guarded = guard_prediction(episode, candidate, context)
    completed = subprocess.run([str(executable), str(history)], check=False,
                               text=True, capture_output=True, timeout=5)
    if completed.returncode != 0:
        raise RuntimeError(f"V0.1 rejected canonical fixture: {completed.stderr}")
    matches = [line for line in completed.stdout.splitlines() if line.startswith("L2_QUERY ")]
    if len(matches) != 1:
        raise RuntimeError("V0.1 query count")
    if guarded:
        return guarded
    fields = dict(token.split("=", 1) for token in matches[0].split()[1:])
    status = fields["status"]
    roles = tuple(fields["roles"].split("|"))
    if len(roles) != 3:
        raise RuntimeError("V0.1 roles")
    if status == "CANDIDATE":
        return Prediction(episode, candidate, status, roles, "SUPPORTED_HISTORY")
    if status == "BLOCKED":
        return blocked(episode, candidate, "RESOURCE_CONSTRAINED")
    reason = "WARMING_UP" if fields["reason"] == "WARMING_UP" else "INVALID_SOURCE"
    return none(episode, candidate, reason)


def load_inputs() -> tuple[list[dict[str, str]], dict[str, dict[str, str]], dict[str, dict[str, object]]]:
    metadata = strict_rows(FIX / "metadata.csv", META_HEADER, 64)
    contexts = strict_rows(FIX / "query_context.csv", CONTEXT_HEADER, 64)
    context_by_id = {row["episode_id"]: row for row in contexts}
    if len(context_by_id) != len(contexts) or set(context_by_id) != {row["episode_id"] for row in metadata}:
        raise ValueError("context pairing")
    states = {row["episode_id"]: inspect_history(FIX / row["history_file"]) for row in metadata}
    return metadata, context_by_id, states


def generate_predictions(output: Path, fixture_cli: Path) -> list[Prediction]:
    metadata, contexts, states = load_inputs()
    predictions: list[Prediction] = []
    for row in metadata:
        episode = row["episode_id"]
        for candidate in ALLOWED_CANDIDATES:
            if candidate == "V01_FROZEN":
                prediction = run_v01(episode, FIX / row["history_file"], contexts[episode], fixture_cli)
            else:
                prediction = run_baseline(candidate, episode, states[episode], contexts[episode])
            predictions.append(prediction)
    with output.open("w", encoding="ascii", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(PREDICTION_HEADER)
        writer.writerows(prediction.row() for prediction in predictions)
    return predictions


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-cli", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    predictions = generate_predictions(args.output, args.fixture_cli.resolve())
    print(f"V02A_BASELINES=PASS episodes=18 predictions={len(predictions)} grid=1|2|3|5|8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
