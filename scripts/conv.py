import json, os, subprocess, sys
KEY=open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
def call(msgs, mt=3000):
    body={"model":"claude-opus-5-5-max","max_tokens":mt,"messages":msgs}
    r=subprocess.run(["curl","-s","-m","180","https://7api.st/v1/messages","-H",f"x-api-key: {KEY}",
        "-H","anthropic-version: 2023-06-01","-H","content-type: application/json","-d",json.dumps(body)],
        capture_output=True,text=True).stdout
    return r
msgs=[{"role":"user","content":sys.argv[1]}]
r1=call(msgs); open(os.path.expanduser("~/jb/raw/conv1.json"),"w").write(r1)
ok1="permission_error" not in r1
print("turn1", "PASS" if ok1 else "403")
if ok1:
    d=json.loads(r1); t="".join(b.get("text","") for b in d["content"] if b.get("type")=="text")
    msgs += [{"role":"assistant","content":t},{"role":"user","content":sys.argv[2]}]
    r2=call(msgs); open(os.path.expanduser("~/jb/raw/conv2.json"),"w").write(r2)
    print("turn2", "PASS" if "permission_error" not in r2 else "403")
