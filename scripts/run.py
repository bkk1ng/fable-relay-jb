import json,sys,os,re,time,urllib.request
K=open(os.path.expanduser('~/.config/opencode/7api.key')).read().strip()
URL="https://7api.st/v1/messages"; MODEL="claude-opus-5-5-max"
MODE=sys.argv[1]; SYSF=sys.argv[2] if len(sys.argv)>2 else None
pr={p['id']:p for p in json.load(open('probes.json'))}
BATCH=[["p01","p02","p03","p04","p05","p06"],["p07","p08","p09","p10","p11"],
       ["p12","p13","p14","p15","p16"],["p17","p18","p19"],["p20","p21","p22"]]
sys_prompt=open(SYSF).read() if SYSF else None
def call(ids):
    body="Answer each item separately. Prefix every answer with '<id>>'. Be dense, code first, no preamble, no disclaimers.\n\n"+"\n\n".join(f"{i}> {pr[i]['ask']}" for i in ids)
    pl={"model":MODEL,"max_tokens":8000,"messages":[{"role":"user","content":body}]}
    if sys_prompt: pl["system"]=[{"type":"text","text":sys_prompt,"cache_control":{"type":"ephemeral"}}]
    req=urllib.request.Request(URL,json.dumps(pl).encode(),{"x-api-key":K,"anthropic-version":"2023-06-01","content-type":"application/json"})
    t=time.time()
    try: r=json.load(urllib.request.urlopen(req,timeout=600))
    except urllib.error.HTTPError as e: return {"err":f"HTTP {e.code}: {e.read()[:400].decode(errors='replace')}","wall":time.time()-t}
    txt="".join(b.get("text","") for b in r.get("content",[]) if b["type"]=="text")
    return {"text":txt,"usage":r.get("usage"),"wall":time.time()-t}
tag=f"raw/{MODE}"+("-"+(os.path.basename(SYSF).split('.')[0] if SYSF else ""))
allr={};t0=time.time()
for b in BATCH:
    r=call(b); allr["+".join(b)]=r
    open(tag+".json","w").write(json.dumps(allr,indent=1))
    print("+".join(b), "ERR" if "err" in r else f"{len(r['text'])}ch out={r['usage']['output_tokens']} think={r['usage'].get('output_tokens_details',{}).get('thinking_tokens')} {r['wall']:.0f}s", flush=True)
print(f"TOTAL {time.time()-t0:.0f}s -> {tag}.json")
