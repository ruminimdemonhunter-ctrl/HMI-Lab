#!/usr/bin/env python3
"""Independent structural/reference verifier for frozen L2 V0.1 fixtures."""

import csv
import hashlib
import io
import json
from collections import Counter
from pathlib import Path

HEADER = ("schema", "seq", "monotonic_ms", "quality", "event", "workspace",
          "role", "activity", "l1_condition", "provenance")
SCHEMA = "HMI_L2_EVENT_V1"
QUALITIES = {"OK", "ABSENT", "INVALID", "RESET"}
EVENTS = {"SESSION_BEGIN", "SESSION_END", "ROLE", "CONTEXT", "QUERY", "GAP", "RESET"}
ROLES = {"NONE", "CODE_EDIT", "BUILD_TEST", "DOC_VIEW", "REFERENCES", "CANVAS", "ASSET_GROUP", "EXPORT", "OTHER"}
ACTIVITIES = {"NONE", "IDLE", "INTERACTIVE", "BACKGROUND", "MULTI_TASK", "UNKNOWN"}
CONDITIONS = {"NONE", "QUIET", "CPU_PRESSURE", "MEMORY_PRESSURE", "IO_PRESSURE", "MIXED_PRESSURE", "UNKNOWN", "UNRELIABLE"}
PRESSURE = {"CPU_PRESSURE", "MEMORY_PRESSURE", "IO_PRESSURE", "MIXED_PRESSURE"}
MAX_U64 = (1 << 64) - 1


class Reject(ValueError):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def number(text, maximum):
    if not text or not text.isascii() or not text.isdecimal():
        raise Reject("decimal grammar")
    value = int(text)
    if value > maximum or str(value) != text:
        raise Reject("decimal range/canonical form")
    return value


def placement(row):
    event = row["event"]; workspace = number(row["workspace"], 4)
    role = row["role"]; activity = row["activity"]; condition = row["l1_condition"]
    if event in {"SESSION_BEGIN", "SESSION_END"}:
        return workspace > 0 and role == activity == condition == "NONE"
    if event == "ROLE":
        return workspace > 0 and role != "NONE" and activity == condition == "NONE"
    if event == "CONTEXT":
        return role == "NONE" and activity != "NONE" and condition != "NONE"
    if event == "QUERY":
        return workspace > 0 and role == activity == condition == "NONE"
    if event == "GAP":
        return workspace == 0 and role == activity == condition == "NONE"
    if event == "RESET":
        return workspace == 0 and role == activity == condition == "NONE"
    return False


def parse(data):
    if len(data) > 16 * 1024 * 1024 or b"\r" in data or b'"' in data or not data.endswith(b"\n"):
        raise Reject("file framing")
    lines = data.splitlines(keepends=True)
    if not lines or any(len(line) > 256 or not line.endswith(b"\n") for line in lines):
        raise Reject("line bound")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise Reject("ASCII") from error
    reader = csv.DictReader(io.StringIO(text))
    if tuple(reader.fieldnames or ()) != HEADER:
        raise Reject("header")
    rows = list(reader)
    if len(rows) > 100000 or any(None in row or None in row.values() for row in rows):
        raise Reject("row shape/count")
    prior_valid_time = None
    for index,row in enumerate(rows, 1):
        if row["schema"] != SCHEMA or number(row["seq"], MAX_U64) != index:
            raise Reject("schema/sequence")
        now = number(row["monotonic_ms"], MAX_U64)
        if row["quality"] not in QUALITIES or row["event"] not in EVENTS or \
                row["role"] not in ROLES or row["activity"] not in ACTIVITIES or \
                row["l1_condition"] not in CONDITIONS or row["provenance"] != "SIMULATED":
            raise Reject("enum/provenance")
        if not placement(row): raise Reject("field placement")
        if (row["quality"] == "RESET") != (row["event"] == "RESET"):
            raise Reject("reset combination")
        if row["quality"] == "OK":
            if prior_valid_time is not None and now <= prior_valid_time:
                raise Reject("valid time regression")
            prior_valid_time = now
    return rows


def analyze(rows):
    sessions = Counter(); transitions = Counter(); nodes = Counter(); edges = Counter(); triples = Counter()
    generation = resets = aborts = 0
    opened = 0; pending = []
    context = "UNKNOWN"; context_time = None; last_valid_time = None
    queries = []; raw_nodes = Counter(); raw_edges = Counter(); raw_triples = Counter()

    def abort():
        nonlocal opened,pending,aborts,context,context_time
        if opened:
            aborts += 1
        opened = 0; pending = []; context = "TRANSITION"; context_time = None

    def age():
        for counter in (nodes,edges,triples,sessions,transitions):
            for key in list(counter):
                counter[key] //= 2
                if counter[key] == 0: del counter[key]

    for row in rows:
        event=row["event"]; quality=row["quality"]; workspace=int(row["workspace"]); now=int(row["monotonic_ms"])
        if quality in {"ABSENT","INVALID"}:
            abort(); last_valid_time = None; continue
        if event == "RESET":
            sessions.clear(); transitions.clear(); nodes.clear(); edges.clear(); triples.clear()
            opened=0; pending=[]; context="UNKNOWN"; context_time=None; last_valid_time=now; generation=0; resets+=1
            continue
        if last_valid_time is not None and now-last_valid_time > 60000:
            abort()
        last_valid_time=now
        if event == "GAP": abort(); continue
        if event == "CONTEXT":
            condition=row["l1_condition"]; activity=row["activity"]
            if condition in {"UNKNOWN","UNRELIABLE"} or activity=="UNKNOWN": context="UNKNOWN"
            elif condition in PRESSURE: context="RESOURCE_CONSTRAINED"
            else: context={"IDLE":"IDLE","INTERACTIVE":"FOCUSED","BACKGROUND":"BACKGROUND_BUSY","MULTI_TASK":"MULTI_TASK"}[activity]
            context_time=now; continue
        if event == "SESSION_BEGIN":
            if opened: raise Reject("nested session")
            opened=workspace; pending=[]; continue
        if event == "ROLE":
            if opened != workspace: raise Reject("role outside matching session")
            if len(pending) >= 8: abort(); continue
            pending.append(row["role"]); continue
        if event == "SESSION_END":
            if opened != workspace: raise Reject("end outside matching session")
            if len(pending) >= 3:
                generation += 1; sessions[workspace] += 1
                for role in pending: nodes[(workspace,role)] += 1; raw_nodes[(workspace,role)] += 1
                for pair in zip(pending,pending[1:]): edges[(workspace,)+pair] += 1; raw_edges[(workspace,)+pair] += 1; transitions[workspace] += 1
                for triple in zip(pending,pending[1:],pending[2:]): triples[(workspace,)+triple] += 1; raw_triples[(workspace,)+triple] += 1
                if generation % 16 == 0: age()
            else: aborts += 1
            opened=0; pending=[]; continue
        if event == "QUERY":
            if opened: abort()
            if context_time is None or now-context_time > 60000:
                queries.append(f"{workspace}:NONE:STALE_CONTEXT"); continue
            supported = sessions[workspace] >= 3 and transitions[workspace] >= 6
            eligible = [(count,key[1:]) for key,count in triples.items() if key[0]==workspace and count>=2]
            if not supported or not eligible:
                queries.append(f"{workspace}:NONE:WARMING_UP"); continue
            if context == "RESOURCE_CONSTRAINED":
                queries.append(f"{workspace}:BLOCKED:RESOURCE_CONSTRAINED"); continue
            best=max(eligible,key=lambda item:(item[0],tuple(-ROLES_ORDER[x] for x in item[1])))
            queries.append(f"{workspace}:CANDIDATE:{'|'.join(best[1])}")
    raw_node_occurrences=sum(raw_nodes.values()); raw_edge_occurrences=sum(raw_edges.values()); raw_triple_occurrences=sum(raw_triples.values())
    return {"sessions":{str(k):v for k,v in sorted(sessions.items())},
            "transitions":{str(k):v for k,v in sorted(transitions.items())},
            "queries":queries,"resets":resets,"aborts":aborts,
            "raw_node_occurrences":raw_node_occurrences,
            "raw_edge_occurrences":raw_edge_occurrences,
            "raw_triple_occurrences":raw_triple_occurrences,
            "unique_nodes":len(raw_nodes),"unique_edges":len(raw_edges),"unique_triples":len(raw_triples)}


ROLES_ORDER = {name:index for index,name in enumerate(("NONE","CODE_EDIT","BUILD_TEST","DOC_VIEW","REFERENCES","CANVAS","ASSET_GROUP","EXPORT","OTHER"))}


def verify_expectation(name, expected, data):
    if expected["parse"] == "REJECT":
        try: parse(data)
        except Reject: return
        raise Reject(f"{name}: expected parse rejection")
    rows=parse(data); actual=analyze(rows)
    for key in ("sessions","transitions","resets","aborts"):
        if key in expected and actual[key] != expected[key]:
            raise Reject(f"{name}: {key} expected={expected[key]} actual={actual[key]}")
    if "queries" in expected:
        wanted=expected["queries"]
        if len(wanted)!=len(actual["queries"]): raise Reject(f"{name}: query count")
        for declared,observed in zip(wanted,actual["queries"]):
            if declared.endswith(":CANDIDATE"):
                if not observed.startswith(declared+":"): raise Reject(f"{name}: candidate query")
            elif declared!=observed: raise Reject(f"{name}: queries expected={wanted} actual={actual['queries']}")
    if "rows" in expected and len(rows) != expected["rows"]: raise Reject(f"{name}: rows")
    mapping={"node_occurrences":"raw_node_occurrences","edge_occurrences":"raw_edge_occurrences","triple_occurrences":"raw_triple_occurrences"}
    for key,actual_key in mapping.items():
        if key in expected and actual[actual_key] != expected[key]: raise Reject(f"{name}: {key}")
    if expected.get("capacity_pressure") and not (actual["unique_nodes"]>16 or actual["unique_edges"]>32 or actual["unique_triples"]>16):
        raise Reject(f"{name}: no capacity pressure")


def expect_reject(label, callback):
    try: callback()
    except Reject: return
    raise SystemExit(f"FAIL: accepted mutation {label}")


def verify_manifest(root):
    manifest=(root/"SHA256SUMS").read_text(encoding="ascii").splitlines()
    seen=set()
    for line in manifest:
        if len(line)<67 or line[64:66]!="  ": raise Reject("manifest grammar")
        digest,path_text=line[:64],line[66:]
        if digest.lower()!=digest or any(c not in "0123456789abcdef" for c in digest) or path_text in seen: raise Reject("manifest identity")
        seen.add(path_text); path=root.parents[2]/path_text
        if not path.is_file() or sha(path.read_bytes())!=digest: raise Reject(f"manifest mismatch {path_text}")


def main():
    root=Path(__file__).resolve().parent/"fixtures_v1"
    verify_manifest(root)
    expectations=json.loads((root/"expectations.json").read_text(encoding="ascii"))
    if expectations.get("schema")!="HMI_L2_FIXTURE_EXPECTATIONS_V1": raise Reject("expectation schema")
    for name,expected in expectations["fixtures"].items(): verify_expectation(name,expected,(root/name).read_bytes())
    a=(root/"a.csv").read_bytes()
    expect_reject("nominal session inflation",lambda: verify_expectation("a.csv",{"parse":"ACCEPT","sessions":{"1":5}},a))
    expect_reject("duplicate sequence",lambda: parse(a.replace(b",2,2000,",b",1,2000,",1)))
    expect_reject("bad provenance",lambda: parse(a.replace(b",SIMULATED\n",b",HOST\n",1)))
    expect_reject("unknown enum",lambda: parse(a.replace(b",QUIET,SIMULATED\n",b",MAGIC,SIMULATED\n",1)))
    reset=(root/"reset_late.csv").read_bytes()
    expect_reject("fabricated post-reset support",lambda: verify_expectation("reset_late.csv",{"parse":"ACCEPT","sessions":{"1":5}},reset))
    invalid=(root/"invalid.csv").read_bytes()
    expect_reject("invalid role admitted",lambda: verify_expectation("invalid.csv",{"parse":"ACCEPT","sessions":{"1":1}},invalid))
    stale=(root/"stale.csv").read_bytes()
    expect_reject("stale candidate",lambda: verify_expectation("stale.csv",{"parse":"ACCEPT","queries":["1:CANDIDATE:CODE_EDIT|BUILD_TEST|DOC_VIEW"]},stale))
    manifest_hash=sha((root/"SHA256SUMS").read_bytes())
    print(f"L2_FIXTURE_VERIFY=PASS fixtures={len(expectations['fixtures'])} negative_mutations=7 manifest_sha256={manifest_hash} heldout=NONE provenance=SIMULATED_ONLY")


if __name__=="__main__":
    main()
