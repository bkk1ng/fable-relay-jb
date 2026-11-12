import json,sys,os,time,urllib.request,urllib.error
K=open(os.path.expanduser('~/.config/opencode/7api.key')).read().strip()
MODE=sys.argv[1]; vs=json.load(open('det2.json'))
for v in vs:
    pl={"model":"claude-opus-5-5-max","max_tokens":300,"messages":[{"role":"user","content":v['ask']}]}
    req=urllib.request.Request("https://7api.st/v1/messages",json.dumps(pl).encode(),{"x-api-key":K,"anthropic-version":"2023-06-01","content-type":"application/json"})
    try:
        r=json.load(urllib.request.urlopen(req,timeout=300))
        t="".join(b.get("text","") for b in r.get("content",[]) if b["type"]=="text")
        print(f"{v['id']} OK   {t[:90]!r}",flush=True)
    except urllib.error.HTTPError as e:
        b=e.read()[:120].decode(errors='replace')
        print(f"{v['id']} {e.code}  {'POLICY' if 'upstream_policy' in b else b[:60]}",flush=True)
