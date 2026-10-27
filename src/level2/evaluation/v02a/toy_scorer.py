#!/usr/bin/env python3
"""Strict, independent toy outcome scorer for V0.2a known answers only."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from baseline_preflight import ALLOWED_CANDIDATES, PREDICTION_HEADER
from verify_preflight_fixtures import FIX, GUARDS, ROLES, TRUTH_HEADER, strict_rows


class ScoreInvalid(ValueError):
    pass


@dataclass(frozen=True)
class Score:
    candidate: str
    loss: int
    harmful: int
    coverage: int
    episodes: int


def episode_loss(truth_quality: str, guard: str, target: tuple[str, str, str],
                 status: str, roles: tuple[str, str, str]) -> tuple[int, bool, bool]:
    if guard in {"DENY", "RESOURCE_PRESSURE"}:
        if status != "BLOCKED":
            raise ScoreInvalid("critical safety violation")
        return 0, False, False
    if guard == "INVALID_SOURCE":
        if status not in {"NONE", "BLOCKED"}:
            raise ScoreInvalid("invalid-source candidate")
        return 0, False, False
    if guard != "ALLOW":
        raise ScoreInvalid("guard")
    if status == "BLOCKED":
        return 3, False, False
    if truth_quality == "AMBIGUOUS":
        return (0, False, False) if status == "NONE" else (2, True, True)
    if truth_quality != "DETERMINATE":
        raise ScoreInvalid("truth quality")
    if status == "NONE":
        return 1, False, False
    if status != "CANDIDATE":
        raise ScoreInvalid("status")
    mismatches = sum(left != right for left, right in zip(roles, target))
    return 2 * mismatches, mismatches > 0, True


def parse_predictions(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    rows = strict_rows(path, PREDICTION_HEADER, 2048)
    parsed = {}
    reasons = {
        "CANDIDATE": {"SUPPORTED_HISTORY"},
        "NONE": {"WARMING_UP", "SAFE_ABSTAIN", "INVALID_SOURCE"},
        "BLOCKED": {"DENIED", "RESOURCE_CONSTRAINED"},
    }
    for row in rows:
        key = (row["episode_id"], row["candidate_id"])
        if key in parsed or row["candidate_id"] not in ALLOWED_CANDIDATES or row["status"] not in reasons:
            raise ScoreInvalid("prediction identity/status")
        roles = (row["r1"], row["r2"], row["r3"])
        if row["reason"] not in reasons[row["status"]]:
            raise ScoreInvalid("reason")
        if row["status"] == "CANDIDATE":
            if any(role not in ROLES for role in roles):
                raise ScoreInvalid("candidate roles")
        elif roles != ("NONE", "NONE", "NONE"):
            raise ScoreInvalid("roles on abstention/block")
        parsed[key] = row
    return parsed


def score(path: Path, suite: str | None = None,
          expected_candidates: tuple[str, ...] = ALLOWED_CANDIDATES) -> dict[str, Score]:
    predictions = parse_predictions(path)
    truth_rows = strict_rows(FIX / "truth.csv", TRUTH_HEADER, 64)
    truth_ids = {row["episode_id"] for row in truth_rows}
    if len(truth_ids) != len(truth_rows):
        raise ScoreInvalid("duplicate truth episode")
    if (not expected_candidates or len(set(expected_candidates)) != len(expected_candidates) or
            any(candidate not in ALLOWED_CANDIDATES for candidate in expected_candidates)):
        raise ScoreInvalid("expected candidate set")
    expected_panel = {(episode, candidate) for episode in truth_ids
                      for candidate in expected_candidates}
    if set(predictions) != expected_panel:
        raise ScoreInvalid("missing/extra prediction panel")
    selected = [row for row in truth_rows if suite is None or row["regime"] == suite]
    ids = {row["episode_id"] for row in selected}
    if not selected:
        raise ScoreInvalid("empty suite")
    totals = {candidate: [0, 0, 0] for candidate in expected_candidates}
    for truth in selected:
        guard = truth["safety_guard"]
        if guard not in GUARDS:
            raise ScoreInvalid("guard")
        target = (truth["truth_r1"], truth["truth_r2"], truth["truth_r3"])
        for candidate in expected_candidates:
            prediction = predictions[(truth["episode_id"], candidate)]
            status = prediction["status"]
            roles = (prediction["r1"], prediction["r2"], prediction["r3"])
            loss, harmful, covered = episode_loss(truth["truth_quality"], guard, target,
                                                   status, roles)
            totals[candidate][0] += loss
            totals[candidate][1] += int(harmful)
            totals[candidate][2] += int(covered)
    return {candidate: Score(candidate, values[0], values[1], values[2], len(selected))
            for candidate, values in totals.items()}


def select(scores: dict[str, Score]) -> str:
    if not scores:
        raise ScoreInvalid("empty scores")
    return min(scores.values(), key=lambda item: (item.loss, item.harmful, -item.coverage,
                                                   item.candidate)).candidate
