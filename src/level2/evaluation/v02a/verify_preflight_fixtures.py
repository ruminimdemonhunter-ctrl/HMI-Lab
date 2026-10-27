#!/usr/bin/env python3
"""Independent strict verifier for the tiny V0.2a preflight fixtures."""

from __future__ import annotations

import csv
import hashlib
import io
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FIX = ROOT / "fixtures"
ROLES = {"CODE_EDIT", "BUILD_TEST", "DOC_VIEW", "REFERENCES", "CANVAS",
         "ASSET_GROUP", "EXPORT", "OTHER"}
TRUTH_QUALITY = {"DETERMINATE", "AMBIGUOUS"}
GUARDS = {"ALLOW", "DENY", "RESOURCE_PRESSURE", "INVALID_SOURCE"}
EVENT_HEADER = ["schema", "seq", "monotonic_ms", "quality", "event", "workspace",
                "role", "activity", "l1_condition", "provenance"]
META_HEADER = ["episode_id", "history_file", "workspace_id", "allowed_query_index",
               "declared_sessions", "declared_transitions", "declared_triple_count", "suite"]
TRUTH_HEADER = ["episode_id", "device_id", "workspace_id", "regime", "allowed_query_index",
                "truth_r1", "truth_r2", "truth_r3", "truth_quality", "safety_guard"]
CONTEXT_HEADER = ["episode_id", "source_quality", "permission", "resource_state", "freshness"]


class Invalid(ValueError):
    pass


def ascii_lines(path: Path, maximum: int = 1 << 20) -> list[str]:
    data = path.read_bytes()
    if not data or len(data) > maximum or not data.endswith(b"\n"):
        raise Invalid("file bounds/newline")
    if any(byte < 0x20 and byte != 0x0A or byte > 0x7E for byte in data):
        raise Invalid("non-ASCII/control byte")
    return data.decode("ascii").splitlines()


def strict_rows(path: Path, header: list[str], limit: int) -> list[dict[str, str]]:
    lines = ascii_lines(path)
    parsed = list(csv.reader(lines, strict=True))
    if not parsed or parsed[0] != header or len(parsed) - 1 > limit:
        raise Invalid("schema/count")
    if any(len(row) != len(header) for row in parsed[1:]):
        raise Invalid("field count")
    return [dict(zip(header, row)) for row in parsed[1:]]


def canonical_uint(text: str, maximum: int) -> int:
    if not text or not text.isascii() or not text.isdecimal() or (len(text) > 1 and text[0] == "0"):
        raise Invalid("noncanonical integer")
    value = int(text)
    if value > maximum:
        raise Invalid("integer range")
    return value


def inspect_history(path: Path) -> dict[str, object]:
    rows = strict_rows(path, EVENT_HEADER, 1000)
    expected_seq = 1
    last_ms = -1
    sessions: list[tuple[int, tuple[str, ...]]] = []
    pending: list[str] | None = None
    pending_workspace = 0
    query_workspace = 0
    guard = "ALLOW"
    for row in rows:
        seq = canonical_uint(row["seq"], (1 << 64) - 1)
        now = canonical_uint(row["monotonic_ms"], (1 << 64) - 1)
        workspace = canonical_uint(row["workspace"], 4)
        if seq != expected_seq or now <= last_ms or row["schema"] != "HMI_L2_EVENT_V1":
            raise Invalid("sequence/time/schema")
        expected_seq += 1
        last_ms = now
        if row["provenance"] != "SIMULATED":
            raise Invalid("provenance")
        kind = row["event"]
        quality = row["quality"]
        if kind == "RESET":
            if quality != "RESET" or workspace != 0:
                raise Invalid("reset")
            sessions.clear()
            pending = None
        elif quality in {"ABSENT", "INVALID"}:
            pending = None
            guard = "INVALID_SOURCE"
        elif quality != "OK":
            raise Invalid("quality")
        elif kind == "SESSION_BEGIN":
            if pending is not None or workspace == 0:
                raise Invalid("nested session")
            pending, pending_workspace = [], workspace
        elif kind == "ROLE":
            if pending is None or workspace != pending_workspace or row["role"] not in ROLES:
                raise Invalid("role placement")
            pending.append(row["role"])
        elif kind == "SESSION_END":
            if pending is None or workspace != pending_workspace:
                raise Invalid("session end")
            if len(pending) >= 3:
                sessions.append((workspace, tuple(pending)))
            pending = None
        elif kind == "CONTEXT":
            if row["l1_condition"] in {"CPU_PRESSURE", "MEMORY_PRESSURE", "IO_PRESSURE", "MIXED_PRESSURE"}:
                guard = "RESOURCE_PRESSURE"
        elif kind == "QUERY":
            if query_workspace or pending is not None or workspace == 0:
                raise Invalid("query")
            query_workspace = workspace
        elif kind == "GAP":
            pending = None
        else:
            raise Invalid("event")
    if not query_workspace or rows[-1]["event"] != "QUERY":
        raise Invalid("missing/final query")
    matching = [roles for workspace, roles in sessions if workspace == query_workspace]
    triples: dict[tuple[str, str, str], int] = {}
    transitions = 0
    for roles in matching:
        transitions += max(0, len(roles) - 1)
        for index in range(max(0, len(roles) - 2)):
            triple = roles[index:index + 3]
            triples[triple] = triples.get(triple, 0) + 1
    return {"rows": len(rows), "workspace": query_workspace, "sessions": len(matching),
            "transitions": transitions, "triple_count": max(triples.values(), default=0),
            "triples": triples, "session_triples": [roles[:3] for roles in matching],
            "guard": guard}


def verify_manifest() -> None:
    lines = ascii_lines(FIX / "SHA256SUMS")
    expected = sorted(path.name for path in FIX.iterdir() if path.name != "SHA256SUMS")
    seen = []
    for line in lines:
        if len(line) < 67 or line[64:66] != "  ":
            raise Invalid("manifest grammar")
        digest, name = line[:64], line[66:]
        if any(ch not in "0123456789abcdef" for ch in digest) or "/" in name or "\\" in name:
            raise Invalid("manifest token")
        if hashlib.sha256((FIX / name).read_bytes()).hexdigest() != digest:
            raise Invalid("manifest digest")
        seen.append(name)
    if seen != expected:
        raise Invalid("manifest membership/order")


def verify_source_manifest() -> None:
    required = ["fixtures/SHA256SUMS", "generate_preflight_fixtures.py",
                "verify_preflight_fixtures.py"]
    seen = []
    for line in ascii_lines(ROOT / "CORPUS_SHA256SUMS"):
        if len(line) < 67 or line[64:66] != "  ":
            raise Invalid("source manifest grammar")
        digest, name = line[:64], line[66:]
        if name not in required or name in seen or any(ch not in "0123456789abcdef" for ch in digest):
            raise Invalid("source manifest membership")
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise Invalid("source manifest digest")
        seen.append(name)
    if seen != required:
        raise Invalid("source manifest order")


def verify(metadata_path: Path = FIX / "metadata.csv", truth_path: Path = FIX / "truth.csv",
           context_path: Path = FIX / "query_context.csv") -> dict[str, dict[str, object]]:
    metadata = strict_rows(metadata_path, META_HEADER, 64)
    truth = strict_rows(truth_path, TRUTH_HEADER, 64)
    contexts = strict_rows(context_path, CONTEXT_HEADER, 64)
    if len({row["episode_id"] for row in metadata}) != len(metadata):
        raise Invalid("duplicate metadata ID")
    if len({row["episode_id"] for row in truth}) != len(truth):
        raise Invalid("duplicate truth ID")
    truth_by_id = {row["episode_id"]: row for row in truth}
    if len({row["episode_id"] for row in contexts}) != len(contexts):
        raise Invalid("duplicate context ID")
    context_by_id = {row["episode_id"]: row for row in contexts}
    if set(truth_by_id) != {row["episode_id"] for row in metadata}:
        raise Invalid("truth join")
    if set(context_by_id) != set(truth_by_id):
        raise Invalid("context join")
    results: dict[str, dict[str, object]] = {}
    identities: dict[str, str] = {}
    for row in metadata:
        episode = row["episode_id"]
        if not episode or not all(ch.isalnum() or ch == "_" for ch in episode):
            raise Invalid("episode ID")
        name = row["history_file"]
        if not name.startswith("history_") or not name.endswith(".csv") or "/" in name or "\\" in name:
            raise Invalid("history path")
        result = inspect_history(FIX / name)
        declared = (canonical_uint(row["workspace_id"], 4),
                    canonical_uint(row["allowed_query_index"], 1000),
                    canonical_uint(row["declared_sessions"], 1000),
                    canonical_uint(row["declared_transitions"], 10000),
                    canonical_uint(row["declared_triple_count"], 1000))
        actual = (result["workspace"], result["rows"], result["sessions"],
                  result["transitions"], result["triple_count"])
        if declared != actual:
            raise Invalid("false declared support")
        target = truth_by_id[episode]
        if canonical_uint(target["workspace_id"], 4) != result["workspace"] or canonical_uint(
                target["allowed_query_index"], 1000) != result["rows"]:
            raise Invalid("truth metadata mismatch")
        if target["truth_quality"] not in TRUTH_QUALITY or target["safety_guard"] not in GUARDS:
            raise Invalid("truth enum")
        if any(target[key] not in ROLES for key in ("truth_r1", "truth_r2", "truth_r3")):
            raise Invalid("truth role")
        context = context_by_id[episode]
        if context["source_quality"] not in {"OK", "INVALID"} or context["permission"] not in {
                "ALLOW", "DENY"} or context["resource_state"] not in {"QUIET", "PRESSURE"} or context[
                "freshness"] not in {"FRESH", "STALE"}:
            raise Invalid("context enum")
        runtime_guard = "INVALID_SOURCE" if context["source_quality"] == "INVALID" else (
            "DENY" if context["permission"] == "DENY" else (
                "RESOURCE_PRESSURE" if context["resource_state"] == "PRESSURE" else "ALLOW"))
        if runtime_guard != target["safety_guard"]:
            raise Invalid("truth/context guard mismatch")
        if result["guard"] != "ALLOW" and result["guard"] != runtime_guard:
            raise Invalid("history/context guard mismatch")
        digest = hashlib.sha256((FIX / name).read_bytes()).hexdigest()
        identities[episode] = digest
        results[episode] = result
    if identities["twin_left"] != identities["twin_right"]:
        raise Invalid("visible twin identity")
    if tuple(truth_by_id["twin_left"][key] for key in ("truth_r1", "truth_r2", "truth_r3")) == tuple(
            truth_by_id["twin_right"][key] for key in ("truth_r1", "truth_r2", "truth_r3")):
        raise Invalid("hidden twin truth")
    return results


def expect_invalid(action) -> None:
    try:
        action()
    except (Invalid, FileNotFoundError):
        return
    raise AssertionError("negative fixture accepted")


def main() -> int:
    verify_source_manifest()
    verify_manifest()
    results = verify()
    assert results["supported_a"]["sessions"] == 3
    assert results["supported_a"]["transitions"] == 6
    assert results["supported_a"]["triple_count"] == 3
    assert results["late_reset"]["sessions"] == 2
    assert results["late_reset"]["transitions"] == 4
    assert results["late_reset"]["triple_count"] == 2
    assert results["resource_pressure"]["guard"] == "RESOURCE_PRESSURE"
    assert results["invalid_source"]["guard"] == "INVALID_SOURCE"
    expect_invalid(lambda: verify(FIX / "negative_false_support.csv", FIX / "truth.csv"))
    expect_invalid(lambda: verify(FIX / "metadata.csv", FIX / "negative_truth_mismatch.csv"))
    expect_invalid(lambda: verify(FIX / "metadata.csv", FIX / "negative_duplicate_truth.csv"))
    expect_invalid(lambda: inspect_history(FIX / "negative_bad_provenance.csv"))
    expect_invalid(lambda: inspect_history(FIX / "negative_truth_leak.csv"))
    print("V02A_FIXTURE_VERIFY=PASS episodes=18 post_reset_sessions=2 "
          "negative_controls=5 twins=NON_IDENTIFIABLE study=NONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
