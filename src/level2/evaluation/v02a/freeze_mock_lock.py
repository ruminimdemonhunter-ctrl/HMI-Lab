#!/usr/bin/env python3
"""Regenerate the deterministic toy prediction and calibration-lock artifacts."""

from __future__ import annotations

import argparse
from pathlib import Path

from baseline_preflight import generate_predictions
from lock_preflight import write_lock
from preflight_runner import BASELINES, selection_digest
from toy_scorer import score, select

HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-cli", required=True, type=Path)
    args = parser.parse_args()
    output = HERE / "locked"
    output.mkdir(exist_ok=True)
    predictions = output / "mock_predictions.csv"
    generate_predictions(predictions, args.fixture_cli.resolve())
    expected = {"cal_recent": "RECENT_1", "cal_static": "STATIC_WORKSPACE",
                "cal_none": "ALWAYS_NONE"}
    score_sets = {}
    for suite, wanted in expected.items():
        scores = {candidate: item for candidate, item in score(predictions, suite).items()
                  if candidate in BASELINES}
        if select(scores) != wanted:
            raise RuntimeError(f"unexpected {suite} winner")
        score_sets[suite] = scores
    digest = selection_digest(score_sets)
    write_lock(output / "calibration.lock", predictions, digest)
    print(f"V02A_MOCK_LOCK_FROZEN digest={digest} study=NONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
