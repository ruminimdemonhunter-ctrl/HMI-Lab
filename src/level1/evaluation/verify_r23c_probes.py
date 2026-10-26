#!/usr/bin/env python3
"""Strict machine-conditioned truth-comparison preflight using canned predictions."""

import csv
import io


class Reject(ValueError):
    pass


OBS_HEADER = "schema,probe,machine,row,time_ms,valid,quality,cpu,memory,io\n"
TRUTH_HEADER = "schema,probe,machine,truth\n"
PRED_HEADER = "schema,probe,machine,prediction,supported,eligible_rows,hit_mask\n"
ALLOWED_OUTCOMES = {"EXPECTED", "SUSPECTED", "UNKNOWN"}
ALLOWED_QUALITIES = {"OK", "ABSENT", "INVALID", "STALE", "REGRESSED", "RESET"}


def rows(text, header):
    if not text.startswith(header):
        raise Reject("schema/header")
    return list(csv.DictReader(io.StringIO(text)))


def decimal(text, maximum):
    if not text or not text.isascii() or not text.isdecimal():
        raise Reject("decimal grammar")
    value = int(text)
    if value > maximum or str(value) != text:
        raise Reject("decimal range/canonical form")
    return value


def validate(observations, truth_text, prediction_text):
    obs = rows(observations, OBS_HEADER); truth = rows(truth_text, TRUTH_HEADER); pred = rows(prediction_text, PRED_HEADER)
    by_probe = {}
    for row in obs:
        if row["schema"] != "R23C_PROBE" or row["machine"] not in {"A", "B"} or \
                row["quality"] not in ALLOWED_QUALITIES or row["valid"] not in {"0", "1"}:
            raise Reject("observation metadata")
        decimal(row["row"], 9); decimal(row["time_ms"], 60_000)
        decimal(row["cpu"], 10_000); decimal(row["memory"], 10_000); decimal(row["io"], 10_000)
        key = (row["probe"], row["machine"], row["row"])
        if key in by_probe: raise Reject("duplicate observation")
        by_probe[key] = row
    probes = {row["probe"] for row in obs}
    if probes != {"A_NATIVE", "B_NATIVE", "INVALID"}: raise Reject("missing probe")
    for probe in probes:
        a = [row for row in obs if row["probe"] == probe and row["machine"] == "A"]
        b = [row for row in obs if row["probe"] == probe and row["machine"] == "B"]
        if len(a) != 10 or len(b) != 10: raise Reject("probe row count")
        if [decimal(row["row"], 9) for row in a] != list(range(10)) or \
                [decimal(row["row"], 9) for row in b] != list(range(10)):
            raise Reject("probe row sequence")
        for left, right in zip(a, b):
            if tuple(left[key] for key in ("row","time_ms","valid","quality","cpu","memory","io")) != \
                    tuple(right[key] for key in ("row","time_ms","valid","quality","cpu","memory","io")):
                raise Reject("A/B observation bytes differ")
    if any(r["schema"] != "R23C_TRUTH" or r["machine"] not in {"A", "B"} or
           r["truth"] not in ALLOWED_OUTCOMES for r in truth):
        raise Reject("truth value")
    expected = {(r["probe"],r["machine"]):r["truth"] for r in truth}
    if len(expected) != len(truth) or set(expected) != {(p,m) for p in probes for m in ("A","B")}:
        raise Reject("truth identity")
    if any(r["schema"] != "R23C_PRED" or r["machine"] not in {"A", "B"} or
           r["prediction"] not in ALLOWED_OUTCOMES or r["supported"] not in {"0", "1"}
           for r in pred):
        raise Reject("prediction value")
    predictions = {(r["probe"],r["machine"]):r for r in pred}
    if len(predictions) != len(pred) or set(predictions) != set(expected): raise Reject("prediction identity")
    for key, outcome in expected.items():
        item = predictions[key]
        observed = [row for row in obs if (row["probe"], row["machine"]) == key]
        prior = None; eligibility = []
        for row in observed:
            now = decimal(row["time_ms"], 60_000)
            eligible = row["valid"] == "1" and row["quality"] == "OK" and \
                (prior is None or now > prior)
            eligibility.append(eligible)
            if eligible: prior = now
        actual_eligible = sum(eligibility)
        declared_eligible = decimal(item["eligible_rows"], 10)
        if declared_eligible != actual_eligible:
            raise Reject("declared eligibility mismatch")
        mask = item["hit_mask"]
        if len(mask) != len(observed) or any(bit not in "01" for bit in mask):
            raise Reject("hit mask grammar")
        if any(bit == "1" and not eligible for bit,eligible in zip(mask,eligibility)):
            raise Reject("hit on ineligible observation")
        hits = mask.count("1")
        if item["supported"] != "1": raise Reject("prediction support")
        derived = "UNKNOWN" if actual_eligible < 10 else \
            ("SUSPECTED" if hits >= 5 else "EXPECTED")
        if item["prediction"] != derived: raise Reject("ineligible prediction")
        if item["prediction"] != outcome: raise Reject("prediction/truth mismatch")
        if key[0] == "INVALID" and (actual_eligible != 0 or outcome != "UNKNOWN"):
            raise Reject("invalid probe truth")


def expect_reject(name, fn):
    try: fn()
    except Reject: return
    raise SystemExit(f"FAIL: Gate D accepted {name}")


def main():
    observations = OBS_HEADER + "".join(
        f"R23C_PROBE,{probe},{machine},{i},{i*1000},{valid},{quality},{cpu},{mem},{io}\n"
        for probe, valid, quality, cpu, mem, io in
        (("A_NATIVE",1,"OK",4000,60,180),("B_NATIVE",1,"OK",600,190,1400),("INVALID",0,"INVALID",0,0,0))
        for machine in ("A","B") for i in range(10))
    truth = TRUTH_HEADER + "".join((
        "R23C_TRUTH,A_NATIVE,A,EXPECTED\n", "R23C_TRUTH,A_NATIVE,B,SUSPECTED\n",
        "R23C_TRUTH,B_NATIVE,A,SUSPECTED\n", "R23C_TRUTH,B_NATIVE,B,EXPECTED\n",
        "R23C_TRUTH,INVALID,A,UNKNOWN\n", "R23C_TRUTH,INVALID,B,UNKNOWN\n"))
    predictions = PRED_HEADER + "".join((
        "R23C_PRED,A_NATIVE,A,EXPECTED,1,10,1111000000\n", "R23C_PRED,A_NATIVE,B,SUSPECTED,1,10,1111100000\n",
        "R23C_PRED,B_NATIVE,A,SUSPECTED,1,10,1111100000\n", "R23C_PRED,B_NATIVE,B,EXPECTED,1,10,1111000000\n",
        "R23C_PRED,INVALID,A,UNKNOWN,1,0,0000000000\n", "R23C_PRED,INVALID,B,UNKNOWN,1,0,0000000000\n"))
    validate(observations, truth, predictions)
    expect_reject("declared eligibility mismatch", lambda: validate(
        observations, truth,
        predictions.replace("A_NATIVE,A,EXPECTED,1,10", "A_NATIVE,A,EXPECTED,1,11")))
    expect_reject("invalid eligibility bypass", lambda: validate(
        observations, truth,
        predictions.replace("INVALID,A,UNKNOWN,1,0", "INVALID,A,UNKNOWN,1,10")))
    expect_reject("invalid truth enum", lambda: validate(
        observations, truth.replace("A_NATIVE,A,EXPECTED", "A_NATIVE,A,MAYBE"), predictions))
    expect_reject("invalid prediction enum", lambda: validate(
        observations, truth, predictions.replace("A_NATIVE,A,EXPECTED", "A_NATIVE,A,MAYBE")))
    expect_reject("hit on ineligible observation", lambda: validate(
        observations, truth, predictions.replace("INVALID,A,UNKNOWN,1,0,0000000000", "INVALID,A,UNKNOWN,1,0,1000000000")))
    expect_reject("swapped truth", lambda: validate(observations, truth.replace("A_NATIVE,B,SUSPECTED","A_NATIVE,B,EXPECTED"), predictions))
    expect_reject("missing probe", lambda: validate(observations.replace("R23C_PROBE,B_NATIVE", "R23C_PROBE,MISSING"), truth, predictions))
    expect_reject("duplicate truth", lambda: validate(observations, truth + "R23C_TRUTH,A_NATIVE,A,EXPECTED\n", predictions))
    expect_reject("distinct inputs", lambda: validate(observations.replace("A_NATIVE,B,0,0,1,OK,4000", "A_NATIVE,B,0,0,1,OK,4001"), truth, predictions))
    expect_reject("unsupported", lambda: validate(observations, truth, predictions.replace("A_NATIVE,A,EXPECTED,1,10", "A_NATIVE,A,EXPECTED,0,10")))
    expect_reject("wrong decision", lambda: validate(observations, truth, predictions.replace("B_NATIVE,A,SUSPECTED", "B_NATIVE,A,EXPECTED")))
    expect_reject("no support", lambda: validate(observations, truth, predictions.replace("B_NATIVE,B,EXPECTED,1,10", "B_NATIVE,B,EXPECTED,1,0")))
    print("R23C_GATE_D=PASS truth_loaded=YES byte_identity=PASS eligibility=recomputed hits=recomputed invalid_eligible=0 novelty_k=5 enums=STRICT positive_pairs=2 invalid_pair=PASS negative_cases=12 predictions=CANNED_TEST_ONLY")


if __name__ == "__main__":
    main()
