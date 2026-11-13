// Verify plugin/fable-relay.ts embeds prompt/PAYLOAD-FABLE.md byte-for-byte at runtime.
// Used by FINAL-REPORT.md's plugin section; exit 1 on drift.
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const grab = (file) => {
  const src = readFileSync(new URL(`../plugin/${file}`, import.meta.url), "utf8");
  const m = src.match(/const PAYLOAD = (`(?:[^`\\]|\\[\s\S])*`)/);
  if (!m) throw new Error(`no PAYLOAD literal in ${file}`);
  return new Function(`return ${m[1]}`)();
};
const md5 = (s) => createHash("md5").update(s).digest("hex");

const checks = [
  ["fable-relay.ts", "../prompt/PAYLOAD-FABLE.md", (s) => s],
  ["fable-relay-cheat.ts", "../prompt/PAYLOAD-CHEAT2.md", (s) => s.replace(/\s+$/, "")],
];

let ok = true;
for (const [plugin, payload, canon] of checks) {
  const embedded = grab(plugin);
  const want = canon(readFileSync(new URL(payload, import.meta.url), "utf8"));
  const same = embedded === want;
  ok &&= same;
  console.log(JSON.stringify({ plugin, payload, embedded_md5: md5(embedded), payload_md5: md5(want),
                               identical: same, len: embedded.length }));
}
process.exit(ok ? 0 : 1);
