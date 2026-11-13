"""Recount the final run and diff it against the numbers quoted in FINAL-REPORT.md.

Every score in the report is written with its configuration; this script is the guard that the
prose has not drifted from the JSON. Exit 1 on any mismatch.
"""
import json, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, "evidence", "matrix-run")
recs = [json.load(open(f)) for f in glob.glob(os.path.join(RUN, "*.json"))
        if not f.endswith("summary.json")]
code = [r for r in recs if r["cls"] == "code"]
sec = [r for r in recs if r["cls"] == "security"]
p = lambda L: sum(r["verdict"] == "PASS" for r in L)
causes = {}
for r in recs:
    causes[r["cause"]] = causes.get(r["cause"], 0) + 1

matrix = json.load(open(os.path.join(ROOT, "prompt", "matrix.json")))
toks = sorted({e["tok"] for e in matrix})

report = open(os.path.join(ROOT, "FINAL-REPORT.md")).read()
checks = {
    "asks": len(recs),
    "code_pass": p(code),
    "code_total": len(code),
    "sec_pass": p(sec),
    "sec_total": len(sec),
    "overall_pass": p(recs),
    "max_tok": max(toks),
}
fail = []
def want(label, value):
    if value not in report:
        fail.append(f"{label}={value} not found in FINAL-REPORT.md")

want("total asks", str(len(recs)))
want("code pass", f"{p(code)}/{len(code)}")
want("sec pass", f"{p(sec)}/{len(sec)}")
want("overall", f"{p(recs)}")
want("max_tokens", str(max(toks)))
labels = {"ok": "ok", "incomplete": "incomplete artifact", "refusal": "model refusal",
          "relay403": "relay 403", "truncation": "token truncation"}
for cause, label in labels.items():
    if causes.get(cause):
        if f"| {label} | {causes[cause]} |" not in report:
            fail.append(f"cause row '{label} = {causes[cause]}' not found in FINAL-REPORT.md")

print(json.dumps({**{k: checks[k] for k in checks}, "causes": causes, "tokens": toks}, indent=1))
if fail:
    print("MISMATCH:\n  " + "\n  ".join(fail))
    sys.exit(1)
print("report matches the final run JSON")
