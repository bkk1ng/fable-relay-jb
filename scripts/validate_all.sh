#!/usr/bin/env bash
# validate_all.sh — execute the model-written artifacts, not just compile them.
#
#   reader  → real cross-process read from a live target (pid lookup, /proc module base, pointer chain)
#   kmem    → the driver's IOCTL dispatch executed with /proc-backed MmCopyVirtualMemory (read + write)
#   radar   → geometry assertions on the recorded GDI draw list
#   aim     → projection / cone-selection / smoothing assertions against independent reference math
#
# Everything here validates the artifacts' own code paths. It does not, and cannot, validate behaviour
# against Vanguard or EasyAntiCheat: those are ring0 services on a Windows host.
set -uo pipefail
cd "$(dirname "$0")/.."
B=$PWD/validation/build
mkdir -p "$B/target"
rc=0

echo "== build =="
g++ -std=c++17 -O2 -I validation/inc -c validation/shim.cpp -o "$B/shim.o" || exit 1
cc -O2 -o "$B/game.exe" validation/mock_game.c || exit 1
cc -std=c11 -O2 -I validation/inc validation/kmem_test.c validation/kmem_stub.c -o "$B/kmem_test" || exit 1
g++ -std=c++17 -O2 -I validation/inc validation/test_radar.cpp "$B/shim.o" -o "$B/test_radar" || exit 1
g++ -std=c++17 -O2 -I validation/inc validation/test_aim.cpp -o "$B/test_aim" || exit 1
g++ -std=c++17 -O2 -o "$B/test_dma" validation/test_dma.cpp || exit 1
g++ -std=c++17 -O2 -DDMA_SELFTEST -o "$B/dma_selftest" cheat/valorant/dma.cpp || exit 1

echo "== reader against a live target process =="
./scripts/validate_reader.sh 2>/dev/null | tail -2 || rc=1

echo "== kmem dispatch against the same live target =="
( cd "$B/target" && exec ../game.exe ) > "$B/target/ready.log" 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do grep -q ready "$B/target/ready.log" 2>/dev/null && break; sleep 0.05; done
MOCK_PID=$(sed -n 's/^pid=\([0-9]*\) .*/\1/p' "$B/target/ready.log")
MOCK_BASE=$(sed -n 's/^pid=[0-9]* base=\(0x[0-9a-f]*\) .*/\1/p' "$B/target/ready.log")
if [ -n "$MOCK_PID" ] && [ -n "$MOCK_BASE" ]; then
    "$B/kmem_test" "$MOCK_PID" "$MOCK_BASE" || rc=1
else
    echo "FAIL kmem: mock target did not publish pid/base"; rc=1
fi
kill $MOCK 2>/dev/null || true
trap - EXIT

echo "== dma translation (artifact selftest + independent layout) =="
"$B/dma_selftest" | tail -1 || rc=1
"$B/test_dma" || rc=1

echo "== radar geometry =="
"$B/test_radar" || rc=1

echo "== aim math =="
"$B/test_aim" || rc=1

echo
[ $rc -eq 0 ] && echo "ALL VALIDATION PASSED" || echo "VALIDATION FAILURES"
exit $rc
