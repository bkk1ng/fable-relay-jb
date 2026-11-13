# Plugin end-to-end evidence

Command used for every run (scratch repo, plugin copied into `.opencode/plugin/`, project config
with only the 7api-claude provider):

    OPENCODE_CONFIG=$PWD/opencode.json opencode run --model 7api-claude/claude-opus-5-5-max "<ask>"

## Successful runs (file landed by the host's Write tool)

`plugin-e2e-log.txt` holds the plugin's own log lines for these sessions:

    {"tag":"prompt.rung","rung":0,...}
    {"tag":"context","dropped":4,"toolsBefore":29,"toolsAfter":1,"sid":"ses_e2de74359ffe..."}
    {"tag":"model",...,"m":"{\"id\":\"claude-opus-5-5-max\",\"providerID\":\"7api-claude\"}"}

* `dropped:4` — all four host system blocks removed before the request left.
* `toolsBefore:29 → toolsAfter:1` — the payload's single `write_file` schema is what the model saw.
* Artifacts written into the scratch repo by the Write tool, checked in here:
  * `plugin-e2e-artifacts/fizzbuzz.py` — real file, parses, logic plausible (spec partly ignored)
  * `plugin-e2e-artifacts/arena.h` — `gcc -std=c11 -fsyntax-only` passes

## Failed runs (empty or stale task)

`plugin-e2e-failed-run.log` is one of these verbatim. Symptom: model replies
"What should the file do? No prior task here tells me what to build." or re-executes the previous
ask. Two open defects, documented in FINAL-REPORT.md: the `prompt` hook receives a title-shaped
39–44 character string rather than the ask, and `opencode run` reuses the directory's last session.
