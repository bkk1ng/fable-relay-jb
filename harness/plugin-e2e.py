#!/usr/bin/env python3
"""plugin-e2e.py — send the plugin's own rewrites to 7api.st and score what comes back.

Reads evidence/rewrite/plugin-asks.json (dumped by harness/plugin-dump.mjs), installs the plugin's
payload (prompt/PAYLOAD-PLUGIN.md) as the system prompt, and sends r0, r1, r2 exactly as the plugin
would. Nothing here rewrites anything: the plugin's output is the input.
"""
import json, os, sys, time
sys.path.insert(0, "harness")
import prims as P

MODEL = "claude-opus-5-5-max"
SYSTEM = open("prompt/FABLE-5.2.txt").read() + "\n\n" + open("prompt/PAYLOAD-PLUGIN.md").read()
asks = json.load(open("evidence/rewrite/plugin-asks.json"))
res = {}
for rung in ("r0", "r1", "r2"):
    t0 = time.time()
    r = P.post(SYSTEM, asks[rung]["text"], 9000)
    code = r.get("code") or ""
    res[rung] = {"verdict": r["verdict"], "chars": len(code), "tok": r.get("out_tokens"),
                 "secs": round(time.time() - t0, 1)}
    print(f"{rung} {r['verdict']:10} {len(code):6} ch  {r.get('out_tokens')} tok  {res[rung]['secs']}s", flush=True)
    if code:
        open(f"evidence/rewrite/plugin.{rung}.cpp", "w").write(code)
    time.sleep(4)
json.dump(res, open("evidence/rewrite/plugin.json", "w"), indent=1)
print(sum(1 for v in res.values() if v["verdict"] == "PASS_TOOL"), "/3 rungs returned code")
