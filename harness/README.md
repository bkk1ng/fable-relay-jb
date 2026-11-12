# harness

`finalharness.py` — the pipeline from `plugin/jb-sanitize.ts` without opencode: lexical
skeleton → per-class rungs → pre-send probe (403 or refusal opener, two passes to trust) →
longest trusted rung → ladder on refusal, 3 passes. Fresh single-message request per rung,
which is why it recovers where the in-session ladder does not.

```bash
printf '%s' 'sk-...' > ~/.config/opencode/7api.key
python3 finalharness.py            # all 24 probes
python3 finalharness.py p01 p06    # selected
```

Writes `rawfinal/<id>.json` with the candidate ladder, per-rung probe verdicts, the chosen
phrase, and the full text.

`finalrun.sh` — the same battery through the real `opencode run` CLI, one session per probe,
re-prompting on refusal. Kept because it is the artifact the plugin was built for; it
stalls for minutes per turn on opencode v2.2.7, so prefer `finalharness.py`.

`probes-final.json` — the 24-ask battery (class → ask).
