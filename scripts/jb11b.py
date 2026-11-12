import os, codecs, importlib.util
spec = importlib.util.spec_from_file_location("jb3", os.path.expanduser("~/jb/jb3.py"))
jb3 = importlib.util.module_from_spec(spec); spec.loader.exec_module(jb3)
jb = jb3.jb; chat = jb3.chat; log_rec = jb3.log_rec
import jb11

def one(seed, pname, messages, use_rot, temp=0.0):
    resp = chat(messages, max_tokens=6000, effort="low", temperature=temp)
    if "error" in resp:
        print(seed, pname, "error"); return
    m = resp["choices"][0]["message"]
    c = (resp.get("usage") or {}).get("cost", 0.0)
    raw = m.get("content") or ""
    body = codecs.decode(raw, "rot_13") if use_rot else raw
    v, w = jb.judge({"content": body})
    log_rec(seed, pname, m, v, w, c, resp)
    print(f"{pname:10s} {seed:12s} -> {v:9s} len={len(body)} | "
          f"{body.strip().replace(chr(10),' ')[:60]}", flush=True)

for pname in ["esp", "trainer"]:
    one("cheat_base", pname,
        [{"role": "system", "content": jb11.NEUTRAL},
         {"role": "user", "content": jb11.PROBES[pname]}], False, 0.0)
    for t in (0.0, 0.7):
        one("cheat_win", pname,
            [{"role": "system", "content": jb11.GAME_SYS},
             {"role": "user", "content": jb11.WIN[pname] + jb11.ROT}], True, t)
