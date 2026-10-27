#!/usr/bin/env python3
"""Finite V0.2a known-answer, adversarial and lock-order preflight."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import tempfile
import time
from pathlib import Path

from baseline_preflight import (GRID, Prediction, generate_predictions, load_inputs,
                                run_baseline)
from lock_preflight import (BOUND, LockInvalid, REPO, SENTINEL, verify_then_open,
                            write_lock)
from toy_scorer import (ScoreInvalid, episode_loss, parse_predictions, score, select)
from verify_preflight_fixtures import (FIX, TRUTH_HEADER, verify, verify_manifest,
                                       verify_source_manifest)

try:
    import resource
except ImportError:  # Windows developer smoke run; qualification is Debian.
    resource = None

A = ("CODE_EDIT", "BUILD_TEST", "DOC_VIEW")
B = ("REFERENCES", "CANVAS", "ASSET_GROUP")
C = ("DOC_VIEW", "REFERENCES", "CANVAS")
BASELINES = {f"RECENT_{n}" for n in GRID} | {"STATIC_WORKSPACE", "STATIC_GLOBAL", "ALWAYS_NONE"}
HERE = Path(__file__).resolve().parent


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def arithmetic_tests() -> None:
    expected = [
        (episode_loss("DETERMINATE", "ALLOW", A, "CANDIDATE", A)[0], 0),
        (episode_loss("DETERMINATE", "ALLOW", A, "CANDIDATE", (B[0], A[1], A[2]))[0], 2),
        (episode_loss("DETERMINATE", "ALLOW", A, "CANDIDATE", (B[0], B[1], A[2]))[0], 4),
        (episode_loss("DETERMINATE", "ALLOW", A, "CANDIDATE", B)[0], 6),
        (episode_loss("DETERMINATE", "ALLOW", A, "NONE", ("NONE",) * 3)[0], 1),
        (episode_loss("AMBIGUOUS", "ALLOW", A, "NONE", ("NONE",) * 3)[0], 0),
        (episode_loss("AMBIGUOUS", "ALLOW", A, "CANDIDATE", A)[0], 2),
        (episode_loss("DETERMINATE", "ALLOW", A, "BLOCKED", ("NONE",) * 3)[0], 3),
    ]
    check(all(actual == wanted for actual, wanted in expected), "toy arithmetic")
    try:
        episode_loss("DETERMINATE", "DENY", A, "CANDIDATE", A)
    except ScoreInvalid:
        pass
    else:
        raise AssertionError("unsafe candidate accepted")
    check(episode_loss("DETERMINATE", "ALLOW", B, "CANDIDATE", A)[0] == 6,
          "wrong B attribution was not caught")


def prediction_negative_tests(predictions: Path, temp: Path) -> int:
    rows = predictions.read_text(encoding="ascii").splitlines()
    mutations = {
        "duplicate": rows + [rows[1]],
        "unknown_candidate": [rows[0], rows[1].replace(",RECENT_1,", ",ORACLE,")],
        "role_on_none": [rows[0], "cold,ALWAYS_NONE,NONE,CODE_EDIT,NONE,NONE,WARMING_UP"],
        "bad_reason": [rows[0], "cold,ALWAYS_NONE,NONE,NONE,NONE,NONE,MADE_UP"],
        "extra_field": [rows[0], rows[1] + ",EXTRA"],
    }
    for name, content in mutations.items():
        path = temp / f"bad_prediction_{name}.csv"
        path.write_text("\n".join(content) + "\n", encoding="ascii", newline="")
        try:
            parse_predictions(path)
        except (ScoreInvalid, ValueError):
            continue
        raise AssertionError(f"prediction mutation accepted: {name}")
    return len(mutations)


def selection_digest(scores_by_suite) -> str:
    rows = []
    for suite in sorted(scores_by_suite):
        for candidate in sorted(scores_by_suite[suite]):
            item = scores_by_suite[suite][candidate]
            rows.append(f"{suite}|{candidate}|{item.loss}|{item.harmful}|{item.coverage}|{item.episodes}\n")
    return hashlib.sha256("".join(rows).encode("ascii")).hexdigest()


def calibration_tests(predictions: Path):
    expected = {"cal_recent": "RECENT_1", "cal_static": "STATIC_WORKSPACE",
                "cal_none": "ALWAYS_NONE"}
    selected = {}
    score_sets = {}
    for suite, wanted in expected.items():
        scores = {candidate: item for candidate, item in score(predictions, suite).items()
                  if candidate in BASELINES}
        winner = select(scores)
        check(winner == wanted, f"{suite}: selected {winner}, expected {wanted}")
        check(sum(item.loss == scores[winner].loss for item in scores.values()) == 1,
              f"{suite}: winner not strict")
        selected[suite] = winner
        score_sets[suite] = scores
    return selected, score_sets, selection_digest(score_sets)


def lock_tests(lock_path: Path, predictions: Path, digest: str, temp: Path) -> tuple[int, int]:
    base = lock_path.read_bytes()
    text = base.decode("ascii").splitlines()
    variants = []
    variants.append(("missing", "\n".join(text[:-1]) + "\n"))
    variants.append(("duplicate", "\n".join(text + [text[-1]]) + "\n"))
    variants.append(("unknown", "\n".join(text + ["unknown=x"]) + "\n"))
    variants.append(("reordered", "\n".join([text[1], text[0], *text[2:]]) + "\n"))
    variants.append(("grid", base.decode("ascii").replace("grid_recent_n=1|2|3|5|8", "grid_recent_n=1|4")))
    variants.append(("model", base.decode("ascii").replace("selected_cal_recent=RECENT_1", "selected_cal_recent=STATIC_GLOBAL")))
    variants.append(("calibration", base.decode("ascii").replace(f"selection_digest={digest}", "selection_digest=" + "0" * 64)))
    variants.append(("hash", base.decode("ascii").replace("predictions_sha256=", "predictions_sha256=" + "f" * 64 + "#")))
    variants.append(("signed_looking", base.decode("ascii").replace("mode=TOY_PREFLIGHT_ONLY", "mode=sha256:deadbeef")))
    rejected = 0
    for name, content in variants:
        candidate = temp / f"bad_lock_{name}.lock"
        candidate.write_text(content, encoding="ascii", newline="")
        attempts: list[Path] = []
        try:
            verify_then_open(candidate, predictions, digest, SENTINEL, attempts)
        except LockInvalid:
            check(not attempts, f"invalid lock opened sentinel: {name}")
            rejected += 1
        else:
            raise AssertionError(f"lock mutation accepted: {name}")

    # Source, ordinary fixture and truth bytes are independently read through a
    # controlled mutation boundary; every rejection must precede sentinel open.
    for name, target in [("source", REPO / "src/level2/l2_state.c"),
                         ("fixture", FIX / "training.csv"),
                         ("truth", FIX / "truth.csv")]:
        def reader(path: Path, target=target) -> bytes:
            data = path.read_bytes()
            return data + b"X" if path.resolve() == target.resolve() else data
        attempts = []
        try:
            verify_then_open(lock_path, predictions, digest, SENTINEL, attempts, reader)
        except LockInvalid:
            check(not attempts, f"mutated {name} opened sentinel")
            rejected += 1
        else:
            raise AssertionError(f"mutated {name} accepted")

    attempts = []
    data = verify_then_open(lock_path, predictions, digest, SENTINEL, attempts)
    check(data == b"HMI_L2_V02A_DUMMY_SENTINEL_NOT_STUDY_DATA\n" and len(attempts) == 1,
          "valid dummy sentinel")
    attempts = []
    try:
        verify_then_open(lock_path, predictions, digest, temp / "not_allowed", attempts)
    except LockInvalid:
        check(not attempts, "unspecified path opened")
        rejected += 1
    else:
        raise AssertionError("unspecified path accepted")
    return rejected, 1


def run_once(fixture_cli: Path, temp: Path):
    verify_source_manifest()
    verify_manifest()
    states = verify()
    predictions = temp / "predictions.csv"
    generated = generate_predictions(predictions, fixture_cli)
    parsed = parse_predictions(predictions)
    check(len(generated) == 162 and len(parsed) == 162, "prediction count")
    by_key = {(item.episode_id, item.candidate_id): item for item in generated}
    expected_v01 = {
        "supported_a": ("CANDIDATE", A), "supported_b": ("CANDIDATE", B),
        "cold": ("NONE", ("NONE",) * 3), "late_reset": ("NONE", ("NONE",) * 3),
        "resource_pressure": ("BLOCKED", ("NONE",) * 3),
        "invalid_source": ("NONE", ("NONE",) * 3), "deny": ("BLOCKED", ("NONE",) * 3),
    }
    for episode, (status, roles) in expected_v01.items():
        prediction = by_key[(episode, "V01_FROZEN")]
        check((prediction.status, prediction.roles) == (status, roles), f"V0.1 {episode}")
    for candidate in {item.candidate_id for item in generated}:
        left = by_key[("twin_left", candidate)]
        right = by_key[("twin_right", candidate)]
        check((left.status, left.roles, left.reason) == (right.status, right.roles, right.reason),
              f"truth leakage: {candidate}")
    metadata, contexts, inspected = load_inputs()
    try:
        run_baseline("RECENT_4", "supported_a", inspected["supported_a"], contexts["supported_a"])
    except ValueError:
        pass
    else:
        raise AssertionError("invalid N accepted")
    arithmetic_tests()
    negative_predictions = prediction_negative_tests(predictions, temp)
    selected, score_sets, digest = calibration_tests(predictions)
    lock_path = temp / "calibration.lock"
    write_lock(lock_path, predictions, digest)
    check(predictions.read_bytes() == (HERE / "locked" / "mock_predictions.csv").read_bytes(),
          "frozen prediction artifact drift")
    check(lock_path.read_bytes() == (HERE / "locked" / "calibration.lock").read_bytes(),
          "frozen calibration lock drift")
    lock_rejections, heldout_opens = lock_tests(lock_path, predictions, digest, temp)
    return predictions, selected, digest, negative_predictions, lock_rejections, heldout_opens


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-cli", required=True, type=Path)
    parser.add_argument("--benchmark-repetitions", type=int, default=1)
    args = parser.parse_args()
    if args.benchmark_repetitions < 1 or args.benchmark_repetitions > 100:
        parser.error("benchmark repetitions must be 1..100")
    fixture_cli = args.fixture_cli.resolve()
    if not fixture_cli.is_file():
        parser.error("fixture CLI not found")
    start = time.process_time_ns()
    final = None
    with tempfile.TemporaryDirectory(prefix="l2-v02a-") as directory:
        temp = Path(directory)
        for _ in range(args.benchmark_repetitions):
            final = run_once(fixture_cli, temp)
    elapsed = time.process_time_ns() - start
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss if resource else 0
    assert final is not None
    _, selected, digest, prediction_negatives, lock_rejections, opens = final
    per_episode = elapsed / (18 * args.benchmark_repetitions)
    print("V02A_SCORER=PASS arithmetic=9 unsafe_hard_fail=PASS "
          f"prediction_negatives={prediction_negatives} wrong_b=CAUGHT")
    print("V02A_CALIBRATION=PASS selected_recent={cal_recent} selected_static={cal_static} "
          "selected_none={cal_none} digest={digest}".format(**selected, digest=digest))
    print(f"V02A_LOCK=PASS tamper_rejections={lock_rejections} invalid_open_attempts=0 "
          f"dummy_open_attempts={opens} sha_authentication=NO")
    print(f"V02A_RESOURCE repetitions={args.benchmark_repetitions} cpu_ns_per_episode={per_episode:.3f} "
          f"runner_rss_kib={rss} embedded_l2_state_bytes=840")
    print("L2_V0_2A_PREFLIGHT=PASS L2_V0_2_STUDY=NOT_RUN L2_UTILITY=UNDETERMINED "
          "L2_APPROPRIATE_INDIVIDUALITY=UNDETERMINED L2_OS_ACTUATION=NOT_AUTHORIZED "
          "PRODUCTION_AUTHORIZATION=BLOCKED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
