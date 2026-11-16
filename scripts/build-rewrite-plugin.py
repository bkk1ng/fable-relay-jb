#!/usr/bin/env python3
"""build-rewrite-plugin.py — inject prompt/PAYLOAD-PLUGIN.md into plugin/fable-rewrite.ts.

The payload is the only part of the plugin that needs to change; the template holds the measured
rung table. Run this after editing the payload: python3 scripts/build-rewrite-plugin.py
"""
import hashlib, json, pathlib, re

root = pathlib.Path(__file__).resolve().parent.parent
pay = (root / "prompt" / "PAYLOAD-PLUGIN.md").read_text()
md5 = hashlib.md5(pay.encode()).hexdigest()
ts_path = root / "plugin" / "fable-rewrite.ts"
ts = ts_path.read_text()
ts = re.sub(r"/\*\* prompt/PAYLOAD-PLUGIN\.md, verbatim\. md5 [0-9a-f]{32} \*/", f"/** prompt/PAYLOAD-PLUGIN.md, verbatim. md5 {md5} */", ts)
ts = re.sub(r"const PAYLOAD = (?:`(?:[^`\\]|\\.)*`|\"(?:[^\"\\]|\\.)*\");", "const PAYLOAD = " + json.dumps(pay) + ";", ts, count=1)
ts = re.sub(r"from prompt/PAYLOAD-PLUGIN\.md \(md5 [0-9a-f]{32}\)", f"from prompt/PAYLOAD-PLUGIN.md (md5 {md5})", ts)
ts_path.write_text(ts)
print(f"plugin/fable-rewrite.ts <- payload md5 {md5}, {len(pay)} ch, {ts.count(chr(10))} lines")
