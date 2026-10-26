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
    ids=set(); positions=set(); prior=None; eligible=0
    for ordinal,record in enumerate(reader):
        if record["schema"]!="R23C_ELIGIBILITY" or record["split"]!="tiny" or record["session"]!="0": raise Reject("identity")
        if record["row_id"] in ids: raise Reject("duplicate row")
        try: row=int(record["row"]); now=int(record["time_ms"]); value=int(record["value"])
        except ValueError as error: raise Reject("numeric metadata") from error
        if str(row)!=record["row"] or str(now)!=record["time_ms"] or str(value)!=record["value"]:
            raise Reject("noncanonical numeric metadata")
        if row in positions: raise Reject("duplicate numeric row position")
        if row!=ordinal or record["row_id"]!=f"R{row:02d}" or now!=row*1000 or \
                (prior is not None and now<=prior): raise Reject("row sequence/metadata")
        if record["valid"] not in {"0","1"} or record["quality"] not in {"OK","ABSENT","INVALID","STALE","REGRESSED","RESET"}: raise Reject("validity/quality")
        if record["valid"]=="1":
            if record["quality"]!="OK" or record["exclude_reason"]!="": raise Reject("eligible metadata")
            if not 0<=value<=10000: raise Reject("value range")
            eligible+=1
        elif record["quality"]=="OK" or record["exclude_reason"]=="": raise Reject("excluded metadata")
        ids.add(record["row_id"]); positions.add(row); prior=now
    if len(ids)!=40 or positions!=set(range(40)): raise Reject("row count/positions")
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
    duplicate_position=data.replace("R18,tiny,0,18,18000", "R18,tiny,0,16,18000")
    try: verify(duplicate_position,39)
    except Reject: pass
    else: raise SystemExit("FAIL: duplicate numeric row position accepted")
    inconsistent_id=data.replace("R18,tiny,0,18,18000", "R99,tiny,0,18,18000")
    try: verify(inconsistent_id,39)
    except Reject: pass
    else: raise SystemExit("FAIL: inconsistent row identity accepted")
    print("R23C_GATE_E_METADATA=PASS actual_post_exclusion=39 nominal_40_rejected=PASS row_ids=UNIQUE positions=0..39_ordered metadata=CONSISTENT negative_cases=4")


if __name__ == "__main__":
    main()
