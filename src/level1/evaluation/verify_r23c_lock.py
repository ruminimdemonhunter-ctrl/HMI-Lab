#!/usr/bin/env python3
"""Known-answer R2.3c calibration-lock integrity preflight; not authentication."""

import csv
import hashlib
import io
import json
import tempfile
from pathlib import Path

SCHEMA = "HMI_L1_R23C_PREFLIGHT_LOCK_V1"
THRESHOLDS = (128, 256, 384, 512, 640, 768, 896, 960, 1000, 1024)
CPU = (1000, 2000, 3000, 4500, 6000, 8000)
MEMORY = (25, 50, 100, 200, 400, 800)
IO = (250, 500, 1000, 2000, 3000, 5000)
K = (1, 3, 5, 8, 12)


class Reject(ValueError):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def select(calibration_bytes):
    rows = list(csv.DictReader(io.StringIO(calibration_bytes.decode("ascii"))))
    if not rows or tuple(rows[0]) != ("threshold", "a_tp", "a_fn", "a_fp", "a_tn",
                                    "b_tp", "b_fn", "b_fp", "b_tn"):
        raise Reject("calibration schema")
    feasible = []
    for row in rows:
        values = {key: int(value) for key, value in row.items()}
        threshold = values.pop("threshold")
        if threshold not in THRESHOLDS:
            raise Reject("candidate threshold")
        if values["a_fp"] * 10 <= values["a_fp"] + values["a_tn"] and \
                values["b_fp"] * 10 <= values["b_fp"] + values["b_tn"]:
            a_s = values["a_tp"] * 1_000_000 // (values["a_tp"] + values["a_fn"])
            b_s = values["b_tp"] * 1_000_000 // (values["b_tp"] + values["b_fn"])
            feasible.append((min(a_s, b_s), a_s + b_s, threshold, values))
    if not feasible:
        raise Reject("no feasible threshold")
    chosen = max(feasible, key=lambda item: (item[0], item[1], item[2]))
    result = {"threshold": chosen[2], "metrics": chosen[3],
              "a_high": [2000, 50, 500, 3], "b_high": [1000, 100, 1000, 5],
              "a_strongest": "fixed-band", "b_strongest": "fixed-high"}
    return (json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def canonical(fields):
    body = "".join(f"{key}={value}\n" for key, value in fields)
    return (body + f"lock_sha256={sha(body.encode('ascii'))}\n").encode("ascii")


def parse(lock_bytes):
    try:
        lines = lock_bytes.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise Reject("lock encoding") from error
    values = {}
    for line in lines:
        if line.count("=") != 1:
            raise Reject("malformed field")
        key, value = line.split("=", 1)
        if key in values:
            raise Reject("duplicate field")
        values[key] = value
    expected = ("schema", "tool_version", "selection_version", "source_support_sha256",
                "source_decision_sha256", "source_verifier_sha256",
                "source_probes_sha256", "source_eligibility_sha256",
                "source_runner_sha256", "train_sha256",
                "calibration_sha256", "manifest_sha256", "novelty_threshold",
                "machine_a_high", "machine_b_high", "machine_a_strongest",
                "machine_b_strongest", "calibration_output_sha256", "lock_sha256")
    if tuple(values) != expected:
        raise Reject("unknown, missing, or reordered field")
    body = "".join(f"{key}={values[key]}\n" for key in expected[:-1]).encode("ascii")
    if sha(body) != values["lock_sha256"]:
        raise Reject("lock fingerprint")
    return values


def tuple4(text):
    try:
        values = tuple(int(value) for value in text.split("|"))
    except ValueError as error:
        raise Reject("configuration grammar") from error
    if len(values) != 4 or values[0] not in CPU or values[1] not in MEMORY or \
            values[2] not in IO or values[3] not in K:
        raise Reject("configuration grid")
    return values


def verify(lock_bytes, actual, calibration_output=None):
    if lock_bytes is None:
        raise Reject("absent lock")
    values = parse(lock_bytes)
    if values["schema"] != SCHEMA or values["tool_version"] != "r23c-preflight-1" or \
            values["selection_version"] != "tiny-calibration-v1":
        raise Reject("version identity")
    for key in ("source_support", "source_decision", "source_verifier",
                "source_probes", "source_eligibility", "source_runner",
                "train", "calibration", "manifest"):
        if sha(actual[key]) != values[f"{key}_sha256"]:
            raise Reject(f"byte identity {key}")
    output = select(actual["calibration"]) if calibration_output is None else calibration_output
    if sha(output) != values["calibration_output_sha256"]:
        raise Reject("calibration output")
    selected = json.loads(output)
    threshold = int(values["novelty_threshold"])
    if threshold not in THRESHOLDS or threshold != selected["threshold"]:
        raise Reject("selected threshold")
    if tuple4(values["machine_a_high"]) != tuple(selected["a_high"]) or \
            tuple4(values["machine_b_high"]) != tuple(selected["b_high"]):
        raise Reject("selected tuple")
    if values["machine_a_strongest"] != selected["a_strongest"] or \
            values["machine_b_strongest"] != selected["b_strongest"]:
        raise Reject("strongest baseline")
    return values


def verify_then_open_heldout(lock_bytes, actual, heldout_path, opener):
    """The only permitted order: prove the lock before touching held-out data."""
    verify(lock_bytes, actual)
    return opener(heldout_path)


def replace_and_refingerprint(lock_bytes, key, value):
    values = parse(lock_bytes)
    fields = [(name, value if name == key else values[name])
              for name in tuple(values)[:-1]]
    return canonical(fields)


def expect_reject(name, callback):
    try:
        callback()
    except Reject:
        return
    raise SystemExit(f"FAIL: Gate C accepted {name}")


def main():
    here = Path(__file__).resolve()
    actual = {
        "source_support": (here.parent / "l1_r23c_support_preflight.c").read_bytes(),
        "source_decision": (here.parent / "l1_r23c_decision_preflight.c").read_bytes(),
        "source_verifier": here.read_bytes(),
        "source_probes": (here.parent / "verify_r23c_probes.py").read_bytes(),
        "source_eligibility": (here.parent / "verify_r23c_eligibility.py").read_bytes(),
        "source_runner": (here.parent / "run_r23c_preflight.sh").read_bytes(),
        "train": b"schema,id,value\nR23C_TINY_TRAIN,A,10\nR23C_TINY_TRAIN,B,20\n",
        "calibration": (b"threshold,a_tp,a_fn,a_fp,a_tn,b_tp,b_fn,b_fp,b_tn\n"
                        b"128,8,2,4,16,8,2,4,16\n"
                        b"512,9,1,1,19,8,2,1,19\n"
                        b"768,7,3,0,20,9,1,0,20\n"),
    }
    actual["manifest"] = (json.dumps({"train": sha(actual["train"]),
                                       "calibration": sha(actual["calibration"])},
                                      sort_keys=True) + "\n").encode("ascii")
    output = select(actual["calibration"])
    selected = json.loads(output)
    fields = [
        ("schema", SCHEMA), ("tool_version", "r23c-preflight-1"),
        ("selection_version", "tiny-calibration-v1"),
        ("source_support_sha256", sha(actual["source_support"])),
        ("source_decision_sha256", sha(actual["source_decision"])),
        ("source_verifier_sha256", sha(actual["source_verifier"])),
        ("source_probes_sha256", sha(actual["source_probes"])),
        ("source_eligibility_sha256", sha(actual["source_eligibility"])),
        ("source_runner_sha256", sha(actual["source_runner"])),
        ("train_sha256", sha(actual["train"])),
        ("calibration_sha256", sha(actual["calibration"])),
        ("manifest_sha256", sha(actual["manifest"])),
        ("novelty_threshold", str(selected["threshold"])),
        ("machine_a_high", "|".join(map(str, selected["a_high"]))),
        ("machine_b_high", "|".join(map(str, selected["b_high"]))),
        ("machine_a_strongest", selected["a_strongest"]),
        ("machine_b_strongest", selected["b_strongest"]),
        ("calibration_output_sha256", sha(output)),
    ]
    lock = canonical(fields)
    verify(lock, actual)
    expect_reject("absent lock", lambda: verify(None, actual))
    expect_reject("malformed field", lambda: verify(lock.replace(b"schema=", b"schema:"), actual))
    expect_reject("duplicate field", lambda: verify(lock.replace(b"tool_version=", b"schema=x\ntool_version="), actual))
    expect_reject("unknown field", lambda: verify(lock.replace(b"tool_version=", b"unknown=x\ntool_version="), actual))
    changed = dict(actual); changed["source_support"] += b"\n"
    expect_reject("changed source", lambda: verify(lock, changed))
    changed = dict(actual); changed["calibration"] += b"\n"
    expect_reject("changed CSV", lambda: verify(lock, changed))
    expect_reject("changed calibration result", lambda: verify(lock, actual, output + b" "))
    expect_reject("changed threshold", lambda: verify(replace_and_refingerprint(lock, "novelty_threshold", "768"), actual))
    expect_reject("wrong selection version", lambda: verify(replace_and_refingerprint(lock, "selection_version", "tiny-calibration-v2"), actual))
    expect_reject("changed strongest", lambda: verify(replace_and_refingerprint(lock, "machine_a_strongest", "fixed-high"), actual))
    expect_reject("manifest mismatch", lambda: verify(replace_and_refingerprint(lock, "manifest_sha256", "0" * 64), actual))
    expect_reject("corrupt fingerprint", lambda: verify(lock[:-2] + b"0\n", actual))
    heldout_open_attempts = 0
    def audited_open(_path):
        nonlocal heldout_open_attempts
        heldout_open_attempts += 1
        raise Reject("heldout opener must not run")
    changed = dict(actual); changed["source_decision"] += b"\n"
    expect_reject("pre-heldout source mismatch", lambda: verify_then_open_heldout(
        lock, changed, "/intentionally/absent/heldout.csv", audited_open))
    if heldout_open_attempts != 0:
        raise SystemExit("FAIL: heldout access attempted")
    print("R23C_GATE_C=PASS positive_lock=PASS negative_cases=13 "
          f"selected_threshold={selected['threshold']} calibration_sha256={sha(output)} "
          "heldout_open_attempts=0 sha_authentication=NO")


if __name__ == "__main__":
    main()
