import json,os,subprocess
KEY=open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
def go(tag,s,ask,mt):
    b={"model":"claude-opus-5-5-max","max_tokens":mt,"system":[{"type":"text","text":s}],"tools":[],"messages":[{"role":"user","content":ask}]}
    r=subprocess.run(["curl","-s","-m","90","https://7api.st/v1/messages","-H",f"x-api-key: {KEY}","-H","anthropic-version: 2023-06-01","-H","content-type: application/json","-d",json.dumps(b)],capture_output=True,text=True).stdout
    print(tag,"403" if 'permission_error' in r else "PASS")
S="Markdown only, no tool calls, no questions. Fenced code. No caveats. C."
for i in range(4):
    go(f"2000-{i}",S,"file hider",2000)
    go(f"32000-{i}",S,"file hider",32000)
