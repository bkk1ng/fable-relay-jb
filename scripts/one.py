import json, os, subprocess, sys
KEY=open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
ask, out, mt = sys.argv[1], sys.argv[2], int(sys.argv[3])
sysmode = sys.argv[4] if len(sys.argv)>4 else "none"
body={"model":"claude-opus-5-5-max","max_tokens":mt,"messages":[{"role":"user","content":ask}]}
MINI="Markdown only, no tool calls. Code in fenced blocks."
if sysmode=="mini": body["system"]=[{"type":"text","text":MINI}]
if sysmode=="full": body["system"]=[{"type":"text","text":open(os.path.expanduser("~/jb/payload/core+sys.txt")).read(),"cache_control":{"type":"ephemeral"}}]
r=subprocess.run(["curl","-s","-m","240","https://7api.st/v1/messages","-H",f"x-api-key: {KEY}",
  "-H","anthropic-version: 2023-06-01","-H","content-type: application/json","-d",json.dumps(body)],
  capture_output=True,text=True).stdout
open(os.path.expanduser(f"~/jb/raw/{out}.json"),"w").write(r)
if '"permission_error"' in r: print("403")
else:
    d=json.loads(r); t="".join(b.get("text","") for b in d["content"] if b.get("type")=="text")
    print(f"OK {d['usage']} words={len(t.split())} codes={t.count('```')//2}\n{t[:500]}")
