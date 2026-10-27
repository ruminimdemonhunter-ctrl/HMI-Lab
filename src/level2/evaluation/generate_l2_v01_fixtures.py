#!/usr/bin/env python3
"""Generate the frozen synthetic-only L2 V0.1 engineering fixtures."""

import hashlib
import json
from pathlib import Path

HEADER = "schema,seq,monotonic_ms,quality,event,workspace,role,activity,l1_condition,provenance\n"
SCHEMA = "HMI_L2_EVENT_V1"
OUT = Path(__file__).resolve().parent / "fixtures_v1"


class Stream:
    def __init__(self):
        self.rows = []
        self.seq = 0
        self.time = 0

    def add(self, event, workspace=0, role="NONE", activity="NONE",
            condition="NONE", quality="OK", provenance="SIMULATED", step=1000):
        self.seq += 1
        self.time += step
        self.rows.append((SCHEMA, self.seq, self.time, quality, event, workspace,
                          role, activity, condition, provenance))

    def context(self, activity="INTERACTIVE", condition="QUIET", workspace=0, step=1000):
        self.add("CONTEXT", workspace, activity=activity, condition=condition, step=step)

    def session(self, workspace, roles, with_context=True):
        if with_context:
            self.context(workspace=workspace)
        self.add("SESSION_BEGIN", workspace)
        for role in roles:
            self.add("ROLE", workspace, role=role)
        self.add("SESSION_END", workspace)

    def query(self, workspace, activity="INTERACTIVE", condition="QUIET", step=1000):
        self.context(activity, condition, workspace, step)
        self.add("QUERY", workspace)

    def bytes(self):
        body = "".join(",".join(map(str, row)) + "\n" for row in self.rows)
        return (HEADER + body).encode("ascii")


A = ("CODE_EDIT", "BUILD_TEST", "DOC_VIEW")
B = ("REFERENCES", "CANVAS", "ASSET_GROUP", "EXPORT")


def fixture_a():
    s = Stream()
    for _ in range(4): s.session(1, A)
    s.query(1)
    return s


def fixture_b():
    s = Stream()
    for _ in range(4): s.session(1, B)
    s.query(1)
    return s


def fixture_mixed():
    s = Stream()
    for _ in range(3): s.session(1, A)
    for _ in range(3): s.session(2, B)
    s.query(1); s.query(2)
    return s


def fixture_cold():
    s = Stream(); s.session(1, A); s.query(1); return s


def fixture_context_only():
    s = Stream(); s.query(1); return s


def fixture_reset_late():
    s = Stream()
    for _ in range(3): s.session(1, A)
    s.add("RESET", quality="RESET")
    for _ in range(2): s.session(1, A)
    s.query(1)
    return s


def fixture_gapped():
    s = Stream(); s.context(workspace=1); s.add("SESSION_BEGIN", 1)
    s.add("ROLE", 1, role="CODE_EDIT"); s.add("GAP")
    s.context(workspace=1); s.add("QUERY", 1)
    return s


def fixture_invalid():
    s = Stream(); s.context(workspace=1); s.add("SESSION_BEGIN", 1)
    s.add("ROLE", 1, role="CODE_EDIT")
    s.add("ROLE", 1, role="BUILD_TEST", quality="INVALID")
    s.context(workspace=1); s.add("QUERY", 1)
    return s


def fixture_stale():
    s = Stream(); s.context(workspace=1); s.add("QUERY", 1, step=60001); return s


def fixture_pressure():
    s = Stream()
    for _ in range(4): s.session(1, A)
    s.query(1, condition="MEMORY_PRESSURE")
    return s


def fixture_capacity():
    s = Stream()
    roles = ("CODE_EDIT", "BUILD_TEST", "DOC_VIEW", "REFERENCES",
             "CANVAS", "ASSET_GROUP", "EXPORT", "OTHER")
    for workspace in range(1, 5):
        for shift in range(5):
            sequence = tuple(roles[(shift + i) % len(roles)] for i in range(5))
            s.session(workspace, sequence, with_context=False)
            s.session(workspace, sequence, with_context=False)
    s.query(4)
    return s


def fixture_saturation():
    s = Stream()
    roles = ("CODE_EDIT",) * 8
    for _ in range(10000):
        s.session(1, roles, with_context=False)
    return s


def fixture_bad_provenance():
    s = Stream(); s.add("CONTEXT", activity="INTERACTIVE", condition="QUIET",
                        provenance="HOST"); return s


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    fixtures = {
        "a.csv": fixture_a(), "b.csv": fixture_b(), "mixed.csv": fixture_mixed(),
        "cold.csv": fixture_cold(), "context_only.csv": fixture_context_only(),
        "reset_late.csv": fixture_reset_late(), "gapped.csv": fixture_gapped(),
        "invalid.csv": fixture_invalid(), "stale.csv": fixture_stale(),
        "pressure_block.csv": fixture_pressure(), "capacity.csv": fixture_capacity(),
        "saturation_100k.csv": fixture_saturation(),
        "bad_provenance.csv": fixture_bad_provenance(),
    }
    OUT.mkdir(parents=True, exist_ok=True)
    for name, stream in fixtures.items():
        (OUT / name).write_bytes(stream.bytes())
    expectations = {
        "schema": "HMI_L2_FIXTURE_EXPECTATIONS_V1",
        "fixtures": {
            "a.csv": {"parse": "ACCEPT", "sessions": {"1": 4}, "transitions": {"1": 8}, "queries": ["1:CANDIDATE:CODE_EDIT|BUILD_TEST|DOC_VIEW"]},
            "b.csv": {"parse": "ACCEPT", "sessions": {"1": 4}, "transitions": {"1": 12}, "queries": ["1:CANDIDATE:REFERENCES|CANVAS|ASSET_GROUP"]},
            "mixed.csv": {"parse": "ACCEPT", "sessions": {"1": 3, "2": 3}, "transitions": {"1": 6, "2": 9}, "queries": ["1:CANDIDATE:CODE_EDIT|BUILD_TEST|DOC_VIEW", "2:CANDIDATE:REFERENCES|CANVAS|ASSET_GROUP"]},
            "cold.csv": {"parse": "ACCEPT", "sessions": {"1": 1}, "transitions": {"1": 2}, "queries": ["1:NONE:WARMING_UP"]},
            "context_only.csv": {"parse": "ACCEPT", "sessions": {}, "transitions": {}, "queries": ["1:NONE:WARMING_UP"]},
            "reset_late.csv": {"parse": "ACCEPT", "sessions": {"1": 2}, "transitions": {"1": 4}, "queries": ["1:NONE:WARMING_UP"], "resets": 1},
            "gapped.csv": {"parse": "ACCEPT", "sessions": {}, "transitions": {}, "queries": ["1:NONE:WARMING_UP"], "aborts": 1},
            "invalid.csv": {"parse": "ACCEPT", "sessions": {}, "transitions": {}, "queries": ["1:NONE:WARMING_UP"], "aborts": 1},
            "stale.csv": {"parse": "ACCEPT", "sessions": {}, "transitions": {}, "queries": ["1:NONE:STALE_CONTEXT"]},
            "pressure_block.csv": {"parse": "ACCEPT", "sessions": {"1": 4}, "transitions": {"1": 8}, "queries": ["1:BLOCKED:RESOURCE_CONSTRAINED"]},
            "capacity.csv": {"parse": "ACCEPT", "capacity_pressure": True, "queries": ["4:CANDIDATE"]},
            "saturation_100k.csv": {"parse": "ACCEPT", "rows": 100000, "node_occurrences": 80000, "edge_occurrences": 70000, "triple_occurrences": 60000},
            "bad_provenance.csv": {"parse": "REJECT"},
        },
    }
    expectation_bytes = (json.dumps(expectations, indent=2, sort_keys=True) + "\n").encode("ascii")
    (OUT / "expectations.json").write_bytes(expectation_bytes)
    tracked = [Path(__file__).resolve(), Path(__file__).resolve().with_name("verify_l2_v01_fixtures.py")]
    tracked += sorted(path for path in OUT.iterdir() if path.name != "SHA256SUMS")
    root = Path(__file__).resolve().parents[2]
    lines = []
    for path in tracked:
        lines.append(f"{sha(path.read_bytes())}  {path.relative_to(root).as_posix()}\n")
    (OUT / "SHA256SUMS").write_text("".join(lines), encoding="ascii", newline="\n")
    print(f"L2_FIXTURE_GENERATE files={len(fixtures)} rows={sum(len(s.rows) for s in fixtures.values())} manifest_sha256={sha((OUT/'SHA256SUMS').read_bytes())}")


if __name__ == "__main__":
    main()
