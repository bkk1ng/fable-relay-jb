// Verify plugin/fable-relay.ts embeds prompt/PAYLOAD-FABLE.md byte-for-byte at runtime.
// Used by FINAL-REPORT.md's plugin section; exit 1 on drift.
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const ts = readFileSync(new URL("../plugin/fable-relay.ts", import.meta.url), "utf8");
const md = readFileSync(new URL("../prompt/PAYLOAD-FABLE.md", import.meta.url), "utf8");
const start = ts.indexOf("const PAYLOAD = `") + "const PAYLOAD = ".length;
const close = ts.indexOf("\n`\n", start);
if (close < 0) throw new Error("unterminated PAYLOAD literal");
const literal = ts.slice(start, close + 2);
const embedded = new Function(`return ${literal}`)();
const md5 = (s) => createHash("md5").update(s).digest("hex");
console.log(JSON.stringify({
  embedded_md5: md5(embedded),
  payload_md5: md5(md),
  identical: embedded === md,
  embedded_len: embedded.length,
  payload_len: md.length,
}, null, 1));
process.exit(embedded === md ? 0 : 1);
