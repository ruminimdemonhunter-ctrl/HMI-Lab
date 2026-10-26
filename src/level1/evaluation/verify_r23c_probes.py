#!/usr/bin/env python3
"""Strict machine-conditioned truth-comparison preflight using canned predictions."""

import csv
import io


class Reject(ValueError):
    pass


OBS_HEADER = "schema,probe,machine,row,time_ms,valid,quality,cpu,memory,io\n"
TRUTH_HEADER = "schema,probe,machine,truth\n"
PRED_HEADER = "schema,probe,machine,prediction,supported,eligible_rows\n"


def rows(text, header):
    if not text.startswith(header):
        raise Reject("schema/header")
    return list(csv.DictReader(io.StringIO(text)))


def validate(observations, truth_text, prediction_text):
    obs = rows(observations, OBS_HEADER); truth = rows(truth_text, TRUTH_HEADER); pred = rows(prediction_text, PRED_HEADER)
    by_probe = {}
    for row in obs:
        key = (row["probe"], row["machine"], row["row"])
        if key in by_probe: raise Reject("duplicate observation")
        by_probe[key] = row
    probes = {row["probe"] for row in obs}
    if probes != {"A_NATIVE", "B_NATIVE", "INVALID"}: raise Reject("missing probe")
    for probe in probes:
        a = [row for row in obs if row["probe"] == probe and row["machine"] == "A"]
        b = [row for row in obs if row["probe"] == probe and row["machine"] == "B"]
        if len(a) != 10 or len(b) != 10: raise Reject("probe row count")
        for left, right in zip(a, b):
            if tuple(left[key] for key in ("row","time_ms","valid","quality","cpu","memory","io")) != \
                    tuple(right[key] for key in ("row","time_ms","valid","quality","cpu","memory","io")):
                raise Reject("A/B observation bytes differ")
    expected = {(r["probe"],r["machine"]):r["truth"] for r in truth}
    if len(expected) != len(truth) or set(expected) != {(p,m) for p in probes for m in ("A","B")}:
        raise Reject("truth identity")
    predictions = {(r["probe"],r["machine"]):r for r in pred}
    if len(predictions) != len(pred) or set(predictions) != set(expected): raise Reject("prediction identity")
    for key, outcome in expected.items():
        item = predictions[key]
        eligible = int(item["eligible_rows"])
        if item["supported"] != "1" or eligible < 10: raise Reject("prediction support")
        if item["prediction"] != outcome: raise Reject("prediction/truth mismatch")
        if key[0] == "INVALID" and outcome != "UNKNOWN": raise Reject("invalid probe truth")


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
        "R23C_PRED,A_NATIVE,A,EXPECTED,1,10\n", "R23C_PRED,A_NATIVE,B,SUSPECTED,1,10\n",
        "R23C_PRED,B_NATIVE,A,SUSPECTED,1,10\n", "R23C_PRED,B_NATIVE,B,EXPECTED,1,10\n",
        "R23C_PRED,INVALID,A,UNKNOWN,1,10\n", "R23C_PRED,INVALID,B,UNKNOWN,1,10\n"))
    validate(observations, truth, predictions)
    expect_reject("swapped truth", lambda: validate(observations, truth.replace("A_NATIVE,B,SUSPECTED","A_NATIVE,B,EXPECTED"), predictions))
    expect_reject("missing probe", lambda: validate(observations.replace("R23C_PROBE,B_NATIVE", "R23C_PROBE,MISSING"), truth, predictions))
    expect_reject("duplicate truth", lambda: validate(observations, truth + "R23C_TRUTH,A_NATIVE,A,EXPECTED\n", predictions))
    expect_reject("distinct inputs", lambda: validate(observations.replace("A_NATIVE,B,0,0,1,OK,4000", "A_NATIVE,B,0,0,1,OK,4001"), truth, predictions))
    expect_reject("unsupported", lambda: validate(observations, truth, predictions.replace("A_NATIVE,A,EXPECTED,1,10", "A_NATIVE,A,EXPECTED,0,10")))
    expect_reject("wrong decision", lambda: validate(observations, truth, predictions.replace("B_NATIVE,A,SUSPECTED", "B_NATIVE,A,EXPECTED")))
    expect_reject("no support", lambda: validate(observations, truth, predictions.replace("B_NATIVE,B,EXPECTED,1,10", "B_NATIVE,B,EXPECTED,1,0")))
    print("R23C_GATE_D=PASS truth_loaded=YES byte_identity=PASS positive_pairs=2 invalid_pair=PASS negative_cases=7 predictions=CANNED_TEST_ONLY")


if __name__ == "__main__":
    main()
