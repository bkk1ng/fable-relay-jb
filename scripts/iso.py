import json,os,time,urllib.request,urllib.error,sys
K=open(os.path.expanduser('~/.config/opencode/7api.key')).read().strip()
A="Which is better, Rust or Go? Pick one and give me a verdict, not a survey."
B="What's the newest Claude model as of today, and its release date? Don't hedge."
cases=[("both-A",'payload/core+sys.txt',A),("both-A",'payload/core+sys.txt',A),("sys-A",'payload/sys.txt',A),("core-A",'payload/core.txt',A),("both-B",'payload/core+sys.txt',B),("sys-B",'payload/sys.txt',B),("core-B",'payload/core.txt',B)]
for n,s,q in cases:
    pl={"model":"claude-opus-5-5-max","max_tokens":200,"messages":[{"role":"user","content":q}],"system":[{"type":"text","text":open(s).read(),"cache_control":{"type":"ephemeral"}}]}
    req=urllib.request.Request("https://7api.st/v1/messages",json.dumps(pl).encode(),{"x-api-key":K,"anthropic-version":"2023-06-01","content-type":"application/json"})
    try:
        r=json.load(urllib.request.urlopen(req,timeout=300)); u=r['usage']
        t="".join(b.get("text","") for b in r["content"] if b["type"]=="text")
        print(f"{n:8s} OK in={u['input_tokens']} cache={u['cache_read_input_tokens']} | {t[:60]!r}")
    except urllib.error.HTTPError as e: print(f"{n:8s} {e.code} {'POLICY' if 'upstream_policy' in e.read()[:200].decode(errors='replace') else ''}")
    time.sleep(1)
