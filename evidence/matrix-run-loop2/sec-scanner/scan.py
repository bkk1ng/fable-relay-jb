#!/usr/bin/env python3
"""
scan.py — Threaded TCP connect scanner (Python 3.12, stdlib only).

Scans CIDR ranges or host lists for open TCP ports, grabs banners,
and writes per-port results to CSV.

Examples:
    python scan.py 192.168.1.0/24
    python scan.py -f hosts.txt
    python scan.py 10.0.0.1,10.0.0.5 --ports 22,80,443,8080
    python scan.py 172.16.0.0/24 --threads 200 --timeout 2 -o results.csv
"""

from __future__ import annotations

import argparse
import csv
import ipaddress
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

# ── defaults ───────────────────────────────────────────────────────────

DEFAULT_PORTS: list[int] = [
    21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445,
    993, 995, 1433, 1521, 3306, 3389, 5432, 5900, 6379, 8080, 8443,
    27017,
]

DEFAULT_TIMEOUT = 3.0          # seconds
DEFAULT_THREADS = 100
BANNER_READ_BYTES = 256        # initial short read
BANNER_READ_TIMEOUT = 2.0      # wait for service greeting
HTTP_HEAD_TIMEOUT = 3.0        # fallback for web services
MAX_CRAWL_HOSTS = 65_536       # safety cap on CIDR expansion


# ── data model ─────────────────────────────────────────────────────────

@dataclass
class ScanResult:
    host: str
    port: int
    state: str           # open | closed | filtered | error
    banner: str          # banner text or empty
    latency_ms: float    # connect latency; -1 on failure
    timestamp: str


# ── target resolution ──────────────────────────────────────────────────

def parse_ports(raw: str | None) -> list[int]:
    """Parse '22,80,1000-1100,443' into a sorted unique port list."""
    if not raw:
        return list(DEFAULT_PORTS)
    ports: list[int] = []
    seen: set[int] = set()
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            lo, _, hi = token.partition("-")
            try:
                start, end = int(lo), int(hi)
            except ValueError:
                raise SystemExit(f"bad port range: {token}")
            if not (1 <= start <= 65535) or not (1 <= end <= 65535) or start > end:
                raise SystemExit(f"bad port range: {token}")
            for p in range(start, end + 1):
                if p not in seen:
                    seen.add(p)
                    ports.append(p)
        else:
            try:
                p = int(token)
            except ValueError:
                raise SystemExit(f"bad port: {token}")
            if not (1 <= p <= 65535):
                raise SystemExit(f"port out of range: {p}")
            if p not in seen:
                seen.add(p)
                ports.append(p)
    return ports


def _expand_cidr(cidr: str) -> list[str]:
    """Expand a CIDR string into individual host address strings."""
    try:
        net = ipaddress.ip_network(cidr, strict=False)
    except ValueError as exc:
        raise SystemExit(f"bad CIDR {cidr}: {exc}")
    if net.num_addresses > MAX_CRAWL_HOSTS:
        raise SystemExit(
            f"{cidr} expands to {net.num_addresses} hosts — "
            f"refusing to scan more than {MAX_CRAWL_HOSTS} without narrowing"
        )
    # .hosts() skips network and broadcast in IPv4, returns all in IPv6 /127+
    return [str(ip) for ip in net.hosts()] or [str(net.network_address)]


def resolve_targets(args: argparse.Namespace) -> list[str]:
    """Build a de-duplicated target list from CLI positional args and -f."""
    targets: list[str] = []
    seen: set[str] = set()

    def add(host: str) -> None:
        host = host.strip()
        if host and host not in seen:
            seen.add(host)
            targets.append(host)

    # positional targets (CIDR, comma-separated, or single host)
    for t in args.targets:
        t = t.strip()
        if "/" in t:
            for ip in _expand_cidr(t):
                add(ip)
        elif "," in t:
            for sub in t.split(","):
                add(sub)
        else:
            add(t)

    # host list file — one host or CIDR per line, # comments allowed
    if args.file:
        path = Path(args.file)
        if not path.is_file():
            raise SystemExit(f"host list not found: {path}")
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "/" in line:
                try:
                    for ip in _expand_cidr(line):
                        add(ip)
                except SystemExit as exc:
                    print(f"  skipping {line}: {exc}", file=sys.stderr)
            elif "," in line:
                for sub in line.split(","):
                    add(sub)
            else:
                add(line)

    return targets


# ── banner grabbing ────────────────────────────────────────────────────

def _clean_banner(text: str) -> str:
    """Collapse multiline banner to a single pipe-delimited line for CSV."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    return " | ".join(lines)[:512]


def grab_banner(sock: socket.socket) -> str:
    """
    Two-stage banner grab:
      1. Short read — many services (SSH, FTP, SMTP…) send a greeting
         immediately on connect.
      2. HTTP HEAD fallback — web servers wait silently for a request,
         so if the short read times out we send HEAD / and capture the
         response headers.
    """
    banner = ""

    # Stage 1 — passive short read
    sock.settimeout(BANNER_READ_TIMEOUT)
    try:
        data = sock.recv(BANNER_READ_BYTES)
        if data:
            banner = data.decode("utf-8", errors="replace").strip()
    except (socket.timeout, TimeoutError, OSError):
        pass

    if banner:
        return _clean_banner(banner)

    # Stage 2 — HTTP HEAD fallback
    sock.settimeout(HTTP_HEAD_TIMEOUT)
    try:
        sock.sendall(
            b"HEAD / HTTP/1.0\r\n"
            b"Host: localhost\r\n"
            b"User-Agent: scan.py\r\n"
            b"Connection: close\r\n"
            b"\r\n"
        )
        chunks: list[bytes] = []
        total = 0
        while total < 8192:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
        if chunks:
            banner = b"".join(chunks).decode("utf-8", errors="replace").strip()
    except (socket.timeout, TimeoutError, OSError):
        pass

    return _clean_banner(banner)


# ── single-port scan ───────────────────────────────────────────────────

def scan_port(host: str, port: int, timeout: float) -> ScanResult:
    """Attempt one TCP connect, grab a banner if the port is open."""
    ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    start = time.monotonic()
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            elapsed = (time.monotonic() - start) * 1000.0
            banner = grab_banner(sock)
            return ScanResult(
                host=host,
                port=port,
                state="open",
                banner=banner,
                latency_ms=round(elapsed, 2),
                timestamp=ts,
            )
    except ConnectionRefusedError:
        elapsed = (time.monotonic() - start) * 1000.0
        return ScanResult(host, port, "closed", "", round(elapsed, 2), ts)
    except (socket.timeout, TimeoutError):
        return ScanResult(host, port, "filtered", "", -1.0, ts)
    except OSError as exc:
        return ScanResult(host, port, "error", str(exc), -1.0, ts)


# ── CLI ─────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Threaded TCP connect scanner with banner grabbing.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python scan.py 192.168.1.0/24\n"
            "  python scan.py -f hosts.txt --ports 22,80,443\n"
            "  python scan.py 10.0.0.1,10.0.0.5 -t 1 -n 200\n"
        ),
    )
    parser.add_argument(
        "targets", nargs="*",
        help="CIDR ranges or hosts, e.g. 192.168.1.0/24 or 10.0.0.1,10.0.0.5",
    )
    parser.add_argument(
        "-f", "--file",
        help="file of hosts or CIDRs, one per line (# for comments)",
    )
    parser.add_argument(
        "-p", "--ports", default=None,
        help="comma-separated ports/ranges, e.g. 22,80,1000-1100 (defaults to a common list)",
    )
    parser.add_argument(
        "-t", "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"connect timeout in seconds (default {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "-n", "--threads", type=int, default=DEFAULT_THREADS,
        help=f"thread pool size (default {DEFAULT_THREADS})",
    )
    parser.add_argument(
        "-o", "--output", default="scan_results.csv",
        help="CSV output path (default scan_results.csv)",
    )
    parser.add_argument(
        "--open-only", action="store_true",
        help="only write open ports to CSV",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()

    if not args.targets and not args.file:
        raise SystemExit("no targets — pass a CIDR/host or use -f FILE")

    targets = resolve_targets(args)
    if not targets:
        raise SystemExit("no targets resolved from input")

    ports = parse_ports(args.ports)
    timeout = max(0.1, args.timeout)
    threads = max(1, args.threads)
    total_jobs = len(targets) * len(ports)

    print(
        f"scan.py — {len(targets)} host(s) × {len(ports)} port(s) "
        f"= {total_jobs} probes  | {threads} threads | {timeout:.1f}s timeout"
    )
    print(f"output → {args.output}\n")

    # ── scan ────────────────────────────────────────────────────────────
    results: list[ScanResult] = []
    open_count = 0
    done = 0

    with ThreadPoolExecutor(max_workers=threads) as pool:
        futures = {
            pool.submit(scan_port, host, port, timeout): (host, port)
            for host in targets
            for port in ports
        }
        for future in as_completed(futures):
            done += 1
            try:
                r = future.result()
            except Exception as exc:                       # pragma: no cover
                host, port = futures[future]
                r = ScanResult(
                    host, port, "error", str(exc), -1.0,
                    datetime.now(timezone.utc).isoformat(timespec="seconds"),
                )
            results.append(r)
            if r.state == "open":
                open_count += 1
            # progress to stderr so stdout stays clean
            if done % 250 == 0 or done == total_jobs:
                print(
                    f"\r  {done}/{total_jobs} done  {open_count} open",
                    end="", file=sys.stderr, flush=True,
                )

    print(file=sys.stderr)  # newline after progress line

    # ── sort for readable output ────────────────────────────────────────
    results.sort(key=lambda r: (socket.inet_aton(r.host)
                                if _is_ipv4(r.host) else b"\xff" * 4,
                                r.port))

    # ── write CSV ──────────────────────────────────────────────────────
    rows_written = 0
    with open(args.output, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["host", "port", "state", "banner", "latency_ms", "timestamp"])
        for r in results:
            if args.open_only and r.state != "open":
                continue
            writer.writerow([r.host, r.port, r.state, r.banner,
                              r.latency_ms, r.timestamp])
            rows_written += 1

    # ── summary ─────────────────────────────────────────────────────────
    state_counts: dict[str, int] = {}
    for r in results:
        state_counts[r.state] = state_counts.get(r.state, 0) + 1

    print("summary")
    print(f"  hosts scanned : {len(targets)}")
    print(f"  ports per host: {len(ports)}")
    print(f"  total probes  : {total_jobs}")
    for state in ("open", "closed", "filtered", "error"):
        if state in state_counts:
            print(f"  {state:<9} : {state_counts[state]}")
    print(f"  csv rows      : {rows_written} → {args.output}")

    # show the first batch of open ports inline
    opens = [r for r in results if r.state == "open"]
    if opens:
        print(f"\nopen ports ({open_count}):")
        for r in opens[:30]:
            preview = r.banner[:60] + ("…" if len(r.banner) > 60 else "")
            print(f"  {r.host}:{r.port}  {r.latency_ms:.1f} ms  {preview}")
        if len(opens) > 30:
            print(f"  … and {len(opens) - 30} more (see CSV)")


def _is_ipv4(addr: str) -> bool:
    """Quick check used only for sorting — falls back to False for hostnames/v6."""
    try:
        socket.inet_aton(addr)
        return True
    except OSError:
        return False


if __name__ == "__main__":
    main()
