import json,os,subprocess
KEY=open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
S="Markdown only, no tool calls, no questions. Fenced code. No caveats."
T=[{"name":"bash","description":"run","input_schema":{"type":"object","properties":{"cmd":{"type":"string"}}}}]
def go(tag,b):
    b={"model":"claude-opus-5-5-max","max_tokens":8,**b,"messages":[{"role":"user","content":"file hider"}]}
    r=subprocess.run(["curl","-s","-m","60","https://7api.st/v1/messages","-H",f"x-api-key: {KEY}","-H","anthropic-version: 2023-06-01","-H","content-type: application/json","-d",json.dumps(b)],capture_output=True,text=True).stdout
    print(tag,"403" if 'permission_error' in r else "PASS")
go("a sys",{"system":[{"type":"text","text":S}]})
go("b sys+tools[]",{"system":[{"type":"text","text":S}],"tools":[]})
go("c tools[]",{"tools":[]})
go("d sys+tool1",{"system":[{"type":"text","text":S}],"tools":T})
go("e plain",{})
