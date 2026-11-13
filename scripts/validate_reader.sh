#!/usr/bin/env bash
# validate_reader.sh — run the model-written reader against a live synthetic target process.
#
# What is exercised: pid lookup by name, module-base resolution from /proc/<pid>/maps, the
# pointer chain (view-matrix pointer → matrix, entity-list pointer → entity pointers), per-field
# decoding with the offsets reader.cpp declares, null-pointer tolerance, and the JSON emitter.
#
# What is not: anything Windows-specific in the syscall path (the shim substitutes /proc) and any
# interaction with Vanguard/EAC, which cannot be simulated here.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
B=$ROOT/validation/build
mkdir -p "$B/target"
rm -f "$B/target/client.dll" "$B/target/expected.json"

# 1. shim + mock target
g++ -std=c++17 -O2 -I validation/inc -c validation/shim.cpp -o "$B/shim.o"
cc -O2 -o "$B/game.exe" validation/mock_game.c

# 2. reader with the target's offsets configured (the only edit; the file's own comment says to set them)
python3 - "$ROOT" <<'PY'
import sys, pathlib
root = pathlib.Path(sys.argv[1])
src = (root / "cheat/valorant/reader.cpp").read_text()
out = (src.replace("constexpr uintptr_t VIEW_MATRIX            = 0x00000000;",
                   "constexpr uintptr_t VIEW_MATRIX            = 0x00001000;")
          .replace("constexpr uintptr_t ENTITY_LIST            = 0x00000000;",
                   "constexpr uintptr_t ENTITY_LIST            = 0x00002000;"))
assert out != src, "offset patch did not apply"
(root / "validation/build/reader_linux.cpp").write_text(out)
PY
g++ -std=c++17 -O2 -I validation/inc "$B/reader_linux.cpp" "$B/shim.o" -o "$B/reader_linux" -pthread

# 3. start the target and wait for it to publish its layout
( cd "$B/target" && exec ../game.exe ) > "$B/target/ready.log" 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do grep -q ready "$B/target/ready.log" 2>/dev/null && break; sleep 0.05; done
grep -q ready "$B/target/ready.log" || { echo "FATAL: mock target did not start"; cat "$B/target/ready.log"; exit 1; }

# 4. run the reader against it
timeout 5 "$B/reader_linux" game.exe > "$B/reader.out" 2> "$B/reader.err" || true
head -1 "$B/reader.out" > "$B/reader.first.json"
test -s "$B/reader.first.json" || { echo "FATAL: reader produced no tick"; cat "$B/reader.err"; exit 1; }

# 5. compare decoded values against what the target actually holds
python3 - "$B" <<'PY'
import json, sys, pathlib
b = pathlib.Path(sys.argv[1])
got = json.loads((b / "reader.first.json").read_text())
exp = json.loads((b / "target/expected.json").read_text())

fails = []
if abs(got["tick_ns"]) < 1000:
    fails.append("tick_ns missing/zero")

vm_got, vm_exp = got["view_matrix"], exp["view_matrix"]
for i, (g, e) in enumerate(zip(vm_got, vm_exp)):
    if abs(g - e) > 1e-4:
        fails.append(f"view_matrix[{i}] {g} != {e}")

if len(got["entities"]) != len(exp["entities"]):
    fails.append(f"entity count {len(got['entities'])} != {len(exp['entities'])} "
                 f"(null slots must be skipped, not emitted)")

for i, (g, e) in enumerate(zip(got["entities"], exp["entities"])):
    for k in ("index", "health", "team", "dormant", "name"):
        if g[k] != e[k]:
            fails.append(f"entity[{i}].{k} {g[k]!r} != {e[k]!r}")
    for ax in ("x", "y", "z"):
        if abs(g["origin"][ax] - e["origin"][ax]) > 1e-4:
            fails.append(f"entity[{i}].origin.{ax} {g['origin'][ax]} != {e['origin'][ax]}")

print(f"reader: {len(got['entities'])} live entities decoded, "
      f"view matrix {len(vm_got)} floats, base from /proc maps")
if fails:
    print("FAIL")
    for f in fails[:12]:
        print("  -", f)
    sys.exit(1)
print("PASS reader — every field matches the target's memory")
PY
