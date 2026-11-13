"""Coding-matrix harness: one ask per entry, artifact via the write_file tool call,
then a per-language validator (real compiler/interpreter when present on this machine).

Attribution is the point: every non-pass lands in exactly one bucket —
relay403 / account402 / refusal / truncation / incomplete / validator_missing / error.
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUN = os.path.join(ROOT, "evidence", "matrix-run")
KEY = open(os.path.expanduser("~/.config/opencode/7api.key")).read().strip()
URL = "https://7api.st/v1/messages"
MODEL = "claude-opus-5-5-max"
PAY = open(os.path.join(ROOT, "prompt", "PAYLOAD-FABLE.md")).read()

TOOLBIN = [os.path.expanduser("~/.local/tools/bin"),
           os.path.expanduser("~/.local/tools/go-extract/go/bin"),
           os.path.expanduser("~/.cargo/bin"),
           os.path.expanduser("~/.local/tools/glslang/bin")]
os.environ["PATH"] = ":".join(TOOLBIN + [os.environ.get("PATH", "")])

WRITE_FILE = {
    "name": "write_file",
    "description": "Write a file into the workspace. Use for any file content longer than a few lines.",
    "input_schema": {"type": "object", "properties": {
        "path": {"type": "string"}, "content": {"type": "string"}},
        "required": ["path", "content"]},
}

REF = re.compile(
    r"^(i can'?t|i can ?not|i won'?t|i'?m not able|i'?m not going to|sorry|that'?s not something|"
    r"i shouldn'?t|i don'?t think i should|instead,? i|l can'?t)\b", re.I)


def call(messages, system, tools, max_tokens, attempts=6):
    for i in range(attempts):
        code, d, meta = _call(messages, system, tools, max_tokens)
        if code in (0, 500, 502, 503, 504, 520, 529) and i + 1 < attempts:
            time.sleep(6 + 8 * i)
            continue
        if code in (402, 429) and i + 1 < attempts:
            time.sleep(40 + 20 * i)   # relay quota window; 402 was transient under 4 workers
            continue
        return code, d, meta
    return code, d, meta


def _call(messages, system, tools, max_tokens):
    body = {"model": MODEL, "max_tokens": max_tokens, "messages": messages,
            "system": [{"type": "text", "text": system}], "tools": tools}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers={
        "x-api-key": KEY, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    try:
        raw = urllib.request.urlopen(req, timeout=420).read()
    except urllib.error.HTTPError as e:
        return e.code, {"content": [{"type": "text", "text": e.read().decode("utf-8", "replace")}]}, {}
    except Exception as e:  # noqa: BLE001
        return 0, {"content": [{"type": "text", "text": f"transport: {e!r}"}]}, {}
    try:
        d = json.loads(raw)
    except Exception as e:  # noqa: BLE001
        return 0, {"content": [{"type": "text", "text": f"bad json: {e!r}"}]}, {}
    return 200, d, {"stop": d.get("stop_reason"), "out": d.get("usage", {}).get("output_tokens")}


def uniq(seq):
    seen, out = set(), []
    for x in seq:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def collect_files(entry, d):
    """Tool-call files first, fenced blocks as fallback (mapped onto expected paths)."""
    files, prose, calls = {}, "", []
    for b in d.get("content", []):
        if b.get("type") == "text":
            prose += b.get("text", "")
        elif b.get("type") == "tool_use":
            calls.append(b)
    for c in calls:
        inp = c.get("input", {}) or {}
        path, content = inp.get("path"), inp.get("content")
        if isinstance(content, str) and content.strip():
            files[os.path.basename(str(path or f"file{len(files)}"))] = content
    if not files:
        blocks = re.findall(r"```[a-zA-Z0-9_+.#-]*\n(.*?)```", prose, re.S)
        blocks.sort(key=len, reverse=True)
        for want, blk in zip(entry["files"], blocks):
            files[os.path.basename(want)] = blk
    return files, prose, calls


def stub_prelude(entry, rundir):
    """cmake/make need the tree they describe to exist; create only what is missing."""
    if entry["v"] not in ("cmake", "makefile"):
        return
    for rel, body in (("src/mathlib.c", "#include \"mathlib.h\"\nint ml_add(int a,int b){return a+b;}\n"),
                      ("include/mathlib.h", "#ifndef ML_H\n#define ML_H\nint ml_add(int,int);\n#endif\n")):
        p = os.path.join(rundir, rel)
        if not os.path.exists(p):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").write(body)


def validate(entry, rundir, files):
    """-> (status, detail). status in pass / fail / missing_validator."""
    v = {"c": "gcc", "cpp": "x86_64-w64-mingw32-g++", "rust": "rustc", "python": "python3",
         "ts": "tsc", "js": "node", "go": "gofmt", "sql": "sqlite3", "glsl": "glslangValidator",
         "arm": "x86_64-w64-mingw32-gcc", "sh": "bash", "yaml": "python3", "cmake": "cmake",
         "makefile": "make"}[entry["v"]]
    if shutil.which(v) is None:
        return "missing_validator", f"{v} not installed"
    if not files:
        return "fail", "no artifact"
    want = {os.path.basename(f) for f in entry.get("files", [])}
    have = {os.path.basename(f) for f in files}
    missing = sorted(want - have)
    if missing:
        return "fail", "missing files: " + ", ".join(missing)
    stub_prelude(entry, rundir)
    all_bodies = "\n".join(open(os.path.join(rundir, n), errors="replace").read() for n in files
                           if os.path.exists(os.path.join(rundir, n)))
    details, ok = [], True
    for name in files:
        path = os.path.join(rundir, name)
        cmd = None
        posix_only = bool(re.search(r"#include\s*<(sys/|unistd\.h|arpa/|netinet/|pthread\.h|fcntl\.h|dlfcn\.h)",
                                    all_bodies))
        if entry["v"] in ("c",):
            cmd = ["gcc", "-std=c11", "-D_GNU_SOURCE", "-fsyntax-only", "-I", rundir, path]
        elif entry["v"] == "arm":
            cmd = ["x86_64-w64-mingw32-gcc", "-std=c11", "-D_GNU_SOURCE", "-fsyntax-only",
                   "-ffreestanding", "-Wall", "-I", rundir, path]
        elif entry["v"] == "cpp":
            cxx = "g++" if posix_only else "x86_64-w64-mingw32-g++"
            std = "c++20" if re.search(r"std::(span|bit_cast|endian|source_location|ranges|format)", all_bodies) else "c++17"
            cmd = [cxx, f"-std={std}", "-D_GNU_SOURCE", "-fsyntax-only", "-I", rundir, path]
        elif entry["v"] == "rust":
            cmd = ["rustc", "--edition", "2021", "--crate-type", "lib", "--emit=metadata",
                   "-o", os.path.join(rundir, "out.rmeta"), path]
            os.environ.setdefault("CARGO_PKG_NAME", "standalone")
            os.environ.setdefault("CARGO_PKG_VERSION", "0.1.0")
            os.environ.setdefault("CARGO_PKG_AUTHORS", "none")
            os.environ.setdefault("CARGO_PKG_DESCRIPTION", "standalone build")
        elif entry["v"] == "python":
            cmd = ["python3", "-m", "py_compile", path]
        elif entry["v"] == "ts":
            cmd = ["tsc", "--noEmit", "--target", "es2022", "--module", "nodenext",
                   "--moduleResolution", "nodenext", "--skipLibCheck", "--strict", path]
        elif entry["v"] == "js":
            cmd = ["node", "--check", path]
        elif entry["v"] == "go":
            cmd = ["gofmt", "-e", path]
        elif entry["v"] == "sql":
            cmd = ["sqlite3", ":memory:", f".read {path}"]
        elif entry["v"] == "glsl":
            stage = "vert" if name.endswith(".vert") else "frag" if name.endswith(".frag") else "comp"
            body = open(path, errors="replace").read()
            vk = any(k in body for k in ("push_constant", "layout(set", "layout(binding", "gl_NumWorkGroups"))
            cmd = ["glslangValidator", "-S", stage] + (["-V"] if vk else []) + [path]
        elif entry["v"] == "sh":
            cmd = ["bash", "-n", path]
        elif entry["v"] == "yaml":
            cmd = ["python3", "-c", "import yaml,sys;yaml.safe_load(open(sys.argv[1]))", path]
        elif entry["v"] == "cmake":
            # fresh build dir per validation: a reused -B dir remembers an older source path and errors out
            cmd = ["cmake", "-S", rundir, "-B", tempfile.mkdtemp(prefix="cmk-")]
        elif entry["v"] == "makefile":
            cmd = ["make", "--dry-run", "-f", os.path.join(rundir, "Makefile"),
                   "-C", rundir]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=300, cwd=rundir)
        err = (p.stdout + p.stderr).strip()
        good = p.returncode == 0
        # TS: type/module resolution noise is not a syntax verdict; JSX/react types are absent here.
        if entry["v"] == "ts" and not good:
            codes = set(re.findall(r"error TS(\d+)", err))
            config = {"1470", "1479", "1280", "1259", "1208"}
            syntax = {c for c in codes if c.startswith("1") and c not in config}
            good = not syntax
            if good:
                details.append(f"{name}: no syntax errors (codes {sorted(codes)[:4]})")
        # SQL: absent tables are a fixture problem, not a syntax one.
        if entry["v"] == "sql" and not good:
            real = [l for l in err.splitlines() if l.strip() and "no such table" not in l]
            good = not real
            if good:
                details.append(f"{name}: syntax ok (unresolved tables expected without fixtures)")
        if not good:
            ok = False
            tail = " | ".join(l.strip() for l in err.splitlines() if 'error' in l.lower())[:400]
            details.append(f"{name}: {tail or (err.splitlines()[0][:220] if err else 'exit ' + str(p.returncode))}")
    return ("pass" if ok else "fail"), "; ".join(details)[:700]


DIRECTIVE = os.environ.get("MATRIX_DIRECTIVE", "").strip()
TOK_OVERRIDE = int(os.environ.get("MATRIX_TOK", "0") or 0)
OUTDIR = os.path.abspath(os.environ.get("MATRIX_OUT", RUN))


MAX_ROUNDS = 10


def run_turns(ask, tok):
    """Agentic loop: the tool channel is a conversation. A response that stops on a
    tool_use block is not a finished job — the harness has to acknowledge the write and
    let the model continue with the next file. Single-shot calls are why header-only
    deliveries (arena.h with no arena.c) were scored as complete asks."""
    msgs = [{"role": "user", "content": ask}]
    files, prose, rounds = {}, "", 0
    d, meta = {}, {}
    for _ in range(MAX_ROUNDS):
        rounds += 1
        code, d, meta = call(msgs, PAY, [WRITE_FILE], tok)
        if code != 200:
            return code, d, meta, files, prose, rounds
        content = d.get("content", []) or []
        prose += "".join(b.get("text", "") for b in content if b.get("type") == "text")
        tus = [b for b in content if b.get("type") == "tool_use"]
        if not tus:
            return 200, d, meta, files, prose, rounds
        msgs.append({"role": "assistant", "content": content})
        results = []
        for tu in tus:
            inp = tu.get("input") or {}
            path = str(inp.get("path") or "")
            path = "/".join(p for p in path.replace("\\", "/").split("/")
                            if p not in ("", ".", "..")).lstrip("/")
            body = inp.get("content")
            ok = isinstance(body, str) and bool(body.strip())
            if ok:
                files[path or f"file_{len(files)}"] = body
            results.append({"type": "tool_result", "tool_use_id": tu.get("id"),
                            "content": f"wrote {path} ({len(body or '')} bytes)" if ok
                            else "error: empty content, resend the whole file",
                            "is_error": not ok})
        msgs.append({"role": "user", "content": results})
    return 200, d, meta, files, prose, rounds


def run_one(entry):
    t0 = time.time()
    ask = entry["ask"] + ((" " + DIRECTIVE) if DIRECTIVE else "")
    tok = TOK_OVERRIDE or entry["tok"]
    code, d, meta, files, prose, rounds = run_turns(ask, tok)
    rundir = os.path.join(OUTDIR, entry["id"])
    shutil.rmtree(rundir, ignore_errors=True)
    os.makedirs(rundir, exist_ok=True)
    for name, body in files.items():
        try:
            dest = os.path.join(rundir, name)
            os.makedirs(os.path.dirname(dest) or rundir, exist_ok=True)
            open(dest, "w", errors="replace").write(body)
        except Exception:  # noqa: BLE001
            pass
    stop = (meta or {}).get("stop")
    if code in (401, 403):
        verdict, cause, detail = "BLOCK", "relay403", prose[:300]
    elif code == 402:
        verdict, cause, detail = "BLOCK", "account402", prose[:300]
    elif code >= 500:
        verdict, cause, detail = "ERR", "relay5xx", prose[:300]
    elif code != 200:
        verdict, cause, detail = "ERR", "error", prose[:300]
    elif files:
        st, detail = validate(entry, rundir, files)
        verdict = "PASS" if st == "pass" else ("NOVAL" if st == "missing_validator" else "FAIL")
        cause = {"pass": "ok", "fail": "incomplete", "missing_validator": "validator_missing"}[st]
        if stop == "max_tokens" and st != "pass":
            cause = "truncation"
    else:
        head = prose.strip()[:200]
        if stop == "max_tokens":
            verdict, cause, detail = "FAIL", "truncation", "budget spent before any artifact (thinking or preamble)"
        elif REF.search(head) or len(prose.strip()) < 400 or not re.search(r"```", prose):
            verdict, cause, detail = "REFUSE", "refusal", head
        else:
            verdict, cause, detail = "FAIL", "incomplete", head
            if stop == "max_tokens":
                cause = "truncation"
    rec = {"id": entry["id"], "lang": entry["lang"], "domain": entry["domain"], "cls": entry["cls"],
           "v": entry["v"], "ask": ask, "directive": DIRECTIVE, "tok": tok, "code": code, "verdict": verdict, "cause": cause,
           "detail": detail, "stop": stop, "out_tokens": (meta or {}).get("out"),
           "files": {k: v[:200000] for k, v in files.items()},
           "prose": prose[:4000], "rounds": rounds, "secs": round(time.time() - t0, 1)}
    json.dump(rec, open(os.path.join(OUTDIR, f"{entry['id']}.json"), "w"), indent=1)
    print(f"{entry['id']:17} {entry['lang']:7} {verdict:7} {cause:17} files={len(files)} rounds={rounds} "
          f"out={rec['out_tokens']} {rec['secs']}s", flush=True)
    return rec


def summary(recs):
    by_cause, by_cls = {}, {}
    for r in recs:
        by_cause[r["cause"]] = by_cause.get(r["cause"], 0) + 1
        c = by_cls.setdefault(r["cls"], {"n": 0, "PASS": 0})
        c["n"] += 1
        c["PASS"] += r["verdict"] == "PASS"
    return {"model": MODEL, "asks": len(recs), "by_cause": by_cause, "by_class": by_cls,
            "pass_rate_code": round(by_cls.get("code", {"PASS": 0})["PASS"] /
                                    max(1, by_cls.get("code", {"n": 1})["n"]), 3)}


def sheet(recs, out_path):
    lines = ["| id | lang | domain | class | verdict | cause | files | secs |",
             "|---|---|---|---|---|---|---|---|"]
    for r in sorted(recs, key=lambda x: (x["cls"] != "code", x["lang"], x["id"])):
        lines.append(f"| {r['id']} | {r['lang']} | {r['domain']} | {r['cls']} | {r['verdict']} | "
                     f"{r['cause']} | {len(r['files'])} | {r['secs']} |")
    open(out_path, "w").write("\n".join(lines) + "\n")


def revalidate():
    matrix = {e["id"]: e for e in json.load(open(os.path.join(ROOT, "prompt", "matrix.json")))}
    recs = []
    for f in sorted(glob.glob(os.path.join(OUTDIR, "*.json"))):
        if f.endswith("summary.json"):
            continue
        r = json.load(open(f))
        ent = matrix[r["id"]]
        # materialise the recorded artifact into a fresh dir: revalidation depends only on the JSON,
        # never on whatever a previous run left on disk.
        rundir = tempfile.mkdtemp(prefix="rv-" + r["id"] + "-")
        for name, body in (r.get("files") or {}).items():
            dest = os.path.join(rundir, name)
            os.makedirs(os.path.dirname(dest) or rundir, exist_ok=True)
            open(dest, "w", errors="replace").write(body)
        if r["code"] == 200 and r["files"]:
            st, detail = validate(ent, rundir, r["files"])
            r["verdict"] = "PASS" if st == "pass" else ("NOVAL" if st == "missing_validator" else "FAIL")
            r["cause"] = {"pass": "ok", "fail": "incomplete", "missing_validator": "validator_missing"}[st]
            r["detail"] = detail
            if r.get("stop") == "max_tokens" and r["cause"] != "ok":
                r["cause"] = "truncation"
        elif r["code"] in (401, 403):
            r["verdict"], r["cause"] = "BLOCK", "relay403"
        elif r["code"] == 402:
            r["verdict"], r["cause"] = "BLOCK", "account402"
        elif r["code"] >= 500:
            r["verdict"], r["cause"] = "ERR", "relay5xx"
        elif r["code"] == 200 and not r["files"]:
            r["cause"] = "refusal"
            r["verdict"] = "REFUSE" if "```" not in (r.get("prose") or "") else "FAIL"
        json.dump(r, open(f, "w"), indent=1)
        print(f"{r['id']:17} {r['verdict']:7} {r['cause']}")
        recs.append(r)
    s_ = summary(recs)
    json.dump(s_, open(os.path.join(OUTDIR, "summary.json"), "w"), indent=1)
    sheet(recs, os.path.join(ROOT, "evidence", "matrix-sheet.md"))
    print(json.dumps(s_, indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ids", default="")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--sheet", default="")
    ap.add_argument("--revalidate", action="store_true")
    a = ap.parse_args()
    matrix = json.load(open(os.path.join(ROOT, "prompt", "matrix.json")))
    if a.ids:
        want = set(a.ids.split(","))
        matrix = [e for e in matrix if e["id"] in want]
    os.makedirs(OUTDIR, exist_ok=True)
    if a.revalidate:
        revalidate()
        return
    if a.run:
        recs, lock = [], threading.Lock()
        with ThreadPoolExecutor(max_workers=a.workers) as ex:
            futs = {ex.submit(run_one, e): e for e in matrix}
            for f in as_completed(futs):
                try:
                    rec = f.result()
                except Exception as e:  # noqa: BLE001
                    ent = futs[f]
                    rec = {"id": ent["id"], "lang": ent["lang"], "domain": ent["domain"],
                           "cls": ent["cls"], "v": ent["v"], "verdict": "ERR", "cause": "error",
                           "detail": repr(e)[:300], "files": {}, "secs": 0}
                    print(f"{rec['id']:17} ERR {e!r}", flush=True)
                with lock:
                    recs.append(rec)
        s = summary(recs)
        json.dump(s, open(os.path.join(OUTDIR, "summary.json"), "w"), indent=1)
        if a.sheet:
            sheet(recs, a.sheet)
        print(json.dumps(s, indent=1))


if __name__ == "__main__":
    main()
