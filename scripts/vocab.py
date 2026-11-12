import json, os, subprocess, sys
KEY=open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
ASKS=[a.strip() for a in sys.argv[1].split("|")]
for i,a in enumerate(ASKS):
    body={"model":"claude-opus-5-5-max","max_tokens":1200,"messages":[{"role":"user","content":a}]}
    r=subprocess.run(["curl","-s","-m","120","https://7api.st/v1/messages","-H",f"x-api-key: {KEY}",
        "-H","anthropic-version: 2023-06-01","-H","content-type: application/json","-d",json.dumps(body)],
        capture_output=True,text=True).stdout
    open(os.path.expanduser(f"~/jb/raw/vocab{i}.json"),"w").write(r)
    ok = "permission_error" not in r and "text" in r
    print(f"{i:2} {'PASS' if ok else '403 '} {a[:80]}")
