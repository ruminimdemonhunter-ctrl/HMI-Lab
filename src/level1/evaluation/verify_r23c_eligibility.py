#!/usr/bin/env python3
"""Independent post-exclusion episode eligibility preflight."""

import csv
import io


class Reject(ValueError):
    pass


def verify(row_text, declared):
    reader = csv.DictReader(io.StringIO(row_text))
    expected = ("schema","row_id","split","session","row","time_ms","valid","quality","value","exclude_reason")
    if tuple(reader.fieldnames or ()) != expected: raise Reject("schema")
    ids=set(); prior=None; eligible=0
    for record in reader:
        if record["schema"]!="R23C_ELIGIBILITY" or record["split"]!="tiny" or record["session"]!="0": raise Reject("identity")
        if record["row_id"] in ids: raise Reject("duplicate row")
        ids.add(record["row_id"]); row=int(record["row"]); now=int(record["time_ms"]); value=int(record["value"])
        good=(record["valid"]=="1" and record["quality"]=="OK" and 0<=value<=10000 and
              (prior is None or now>prior) and record["exclude_reason"]=="")
        if good: eligible+=1; prior=now
        elif record["exclude_reason"]=="": raise Reject("missing exclusion reason")
        if row<0 or row>=40: raise Reject("row range")
    if len(ids)!=40: raise Reject("row count")
    if eligible!=declared: raise Reject(f"eligibility declared={declared} actual={eligible}")
    return eligible


def main():
    header="schema,row_id,split,session,row,time_ms,valid,quality,value,exclude_reason\n"
    data=header+"".join(f"R23C_ELIGIBILITY,R{i:02d},tiny,0,{i},{i*1000},{0 if i==17 else 1},{'ABSENT' if i==17 else 'OK'},10,{'absent' if i==17 else ''}\n" for i in range(40))
    if verify(data,39)!=39: raise SystemExit("FAIL: positive eligibility")
    try: verify(data,40)
    except Reject: pass
    else: raise SystemExit("FAIL: nominal pre-fault count accepted")
    duplicated=data.replace("R18,tiny", "R16,tiny")
    try: verify(duplicated,39)
    except Reject: pass
    else: raise SystemExit("FAIL: duplicate row accepted")
    print("R23C_GATE_E_METADATA=PASS actual_post_exclusion=39 nominal_40_rejected=PASS duplicate_rejected=PASS")


if __name__ == "__main__":
    main()
