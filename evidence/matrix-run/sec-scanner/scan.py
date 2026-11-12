#!/usr/bin/env python3
"""
scan.py — Threaded TCP connect scanner
Python 3.12, stdlib only

Usage:
    python scan.py --targets 192.168.1.0/24 --threads 100 --timeout 1.5 --output results.csv
    python scan.py --targets hosts.txt --ports 22,80,443,8080 --threads 50 --output results.csv
"""

import argparse
import csv
import ipaddress
import socket
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

COMMON_PORTS: list[int] = [
    21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445,
    465, 587, 993, 995, 1080, 1433, 1521, 2375, 2376, 3000, 3306,
    3389, 4443, 5000, 5432, 5900, 6379, 6443, 7443, 8000, 8080,
    8081, 8443, 8888, 9000, 9090, 9200, 9443, 27017,
]

BANNER_READ_BYTES = 1024
BANNER_TIMEOUT    = 2.0   # seconds for banner read after connect
HTTP_TIMEOUT      = 3.0   # seconds for HTTP HEAD fallback
DEFAULT_TIMEOUT   = 1.5   # connect timeout
DEFAULT_THREADS   = 200

# ---------------------------------------------------------------------------
# Target expansion
# ---------------------------------------------------------------------------

def expand_targets(source: str) -> list[str]:
    """
    Accept a CIDR range, a single host/IP, or a path to a file containing
    one entry per line (CIDRs and hostnames both allowed).
    Returns a flat list of string addresses.
    """
    path = Path(source)
    if path.is_file():
        lines = [l.strip() for l in path.read_text().splitlines() if l.strip() and not l.startswith("#")]
    else:
        lines = [source.strip()]

    hosts: list[str] = []
    for line in lines:
        try:
            network = ipaddress.ip_network(line, strict=False)
            hosts.extend(str(ip) for ip in network.hosts())
        except ValueError:
            hosts.append(line)   # treat as a hostname
    return hosts


# ---------------------------------------------------------------------------
# Port parsing
# ---------------------------------------------------------------------------

def parse_ports(spec: str | None) -> list[int]:
    """
    Parse a comma-separated list of ports / ranges like "22,80,443,8000-8010".
    Returns COMMON_PORTS when spec is None or empty.
    """
    if not spec:
        return COMMON_PORTS

    ports: set[int] = set()
    for token in spec.split(","):
        token = token.strip()
        if "-" in token:
            lo, hi = token.split("-", 1)
            ports.update(range(int(lo), int(hi) + 1))
        else:
            ports.add(int(token))
    return sorted(ports)


# ---------------------------------------------------------------------------
# Banner / service detection
# ---------------------------------------------------------------------------

def _try_banner(host: str, port: int) -> str:
    """
    Attempt a raw read immediately after connect.
    Many services (SSH, FTP, SMTP, …) push a banner on connect.
    """
    try:
        with socket.create_connection((host, port), timeout=BANNER_TIMEOUT) as s:
            s.settimeout(BANNER_TIMEOUT)
            data = s.recv(BANNER_READ_BYTES)
            return data.decode("utf-8", errors="replace").strip()
    except Exception:
        return ""


def _try_http_head(host: str, port: int) -> str:
    """
    Send a minimal HTTP HEAD / HTTP/1.0 request and return the status line
    plus selected headers. Used as a fallback when no passive banner appears.
    """
    request = f"HEAD / HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    try:
        with socket.create_connection((host, port), timeout=HTTP_TIMEOUT) as s:
            s.settimeout(HTTP_TIMEOUT)
            s.sendall(request.encode())
            data = b""
            while True:
                chunk = s.recv(1024)
                if not chunk:
                    break
                data += chunk
                if len(data) > 4096:
                    break
            text = data.decode("utf-8", errors="replace").strip()
            # Return only the status line + Server / Location headers to keep
            # the CSV readable.
            lines = text.splitlines()
            interesting = [l for l in lines if l.startswith(("HTTP/", "Server:", "Location:"))]
            return " | ".join(interesting[:5])
    except Exception:
        return ""


def grab_banner(host: str, port: int) -> str:
    """
    Try passive banner first; fall back to HTTP HEAD for ports that look
    web-ish or when no banner is received.
    """
    banner = _try_banner(host, port)
    if banner:
        return banner[:256]   # truncate to keep CSV tidy

    # HTTP HEAD fallback for common web ports or any port that returned nothing
    if port in {80, 443, 8000, 8080, 8081, 8443, 8888, 9000, 9090, 9443, 4443, 7443, 3000, 5000}:
        http_banner = _try_http_head(host, port)
        if http_banner:
            return http_banner[:256]

    return ""


# ---------------------------------------------------------------------------
# Single scan task
# ---------------------------------------------------------------------------

def scan_port(host: str, port: int, timeout: float) -> dict:
    """
    Attempt a TCP connect to host:port.
    Returns a dict with keys: host, port, state, banner, latency_ms.
    """
    result = {
        "host":       host,
        "port":       port,
        "state":      "closed",
        "banner":     "",
        "latency_ms": "",
    }
    t0 = time.monotonic()
    try:
        with socket.create_connection((host, port), timeout=timeout):
            latency = (time.monotonic() - t0) * 1000
            result["state"]      = "open"
            result["latency_ms"] = f"{latency:.1f}"
            result["banner"]     = grab_banner(host, port)
    except (ConnectionRefusedError, socket.timeout, TimeoutError):
        pass
    except OSError:
        result["state"] = "filtered"
    return result


# ---------------------------------------------------------------------------
# CSV writer
# ---------------------------------------------------------------------------

CSV_FIELDS = ["host", "port", "state", "banner", "latency_ms"]


def write_results(rows: list[dict], output_path: str) -> None:
    with open(output_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def print_summary(rows: list[dict]) -> None:
    open_rows = [r for r in rows if r["state"] == "open"]
    counts: dict[int, int] = defaultdict(int)
    for r in open_rows:
        counts[r["port"]] += 1

    print(f"\n{'─' * 52}")
    print(f"  Total probes : {len(rows)}")
    print(f"  Open         : {len(open_rows)}")
    print(f"  Closed       : {sum(1 for r in rows if r['state'] == 'closed')}")
    print(f"  Filtered     : {sum(1 for r in rows if r['state'] == 'filtered')}")
    if counts:
        print(f"\n  Open port counts:")
        for port in sorted(counts):
            print(f"    {port:<6} {counts[port]} host(s)")
    print(f"{'─' * 52}\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Threaded TCP connect scanner — CIDR/host-list → CSV",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "--targets", "-t",
        required=True,
        help="CIDR range, single host, or path to a file with one host/CIDR per line.",
    )
    p.add_argument(
        "--ports", "-p",
        default=None,
        help=(
            "Comma-separated ports or ranges (e.g. 22,80,443,8000-8010). "
            "Omit to use the built-in common-port list."
        ),
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        metavar="SECONDS",
        help="TCP connect timeout per port.",
    )
    p.add_argument(
        "--threads",
        type=int,
        default=DEFAULT_THREADS,
        metavar="N",
        help="Thread pool size.",
    )
    p.add_argument(
        "--output", "-o",
        default="scan_results.csv",
        help="CSV output file path.",
    )
    p.add_argument(
        "--open-only",
        action="store_true",
        help="Write only open ports to the CSV (closed/filtered rows are still counted in the summary).",
    )
    return p


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = build_parser()
    args   = parser.parse_args()

    targets = expand_targets(args.targets)
    ports   = parse_ports(args.ports)

    total_tasks = len(targets) * len(ports)
    print(
        f"Scanning {len(targets)} host(s) × {len(ports)} port(s) "
        f"= {total_tasks} probes  |  threads={args.threads}  timeout={args.timeout}s"
    )

    all_results: list[dict] = []
    completed   = 0

    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        futures = {
            pool.submit(scan_port, host, port, args.timeout): (host, port)
            for host in targets
            for port in ports
        }
        for future in as_completed(futures):
            completed += 1
            result = future.result()
            all_results.append(result)

            if result["state"] == "open":
                print(
                    f"  OPEN  {result['host']:>45}:{result['port']:<6}"
                    f"  {result['latency_ms']:>7} ms"
                    f"  {result['banner'][:60]}"
                )

            # Progress tick every 500 completions
            if completed % 500 == 0:
                pct = completed / total_tasks * 100
                print(f"  ... {completed}/{total_tasks} ({pct:.0f}%)")

    # Sort: open first, then by host / port
    state_order = {"open": 0, "filtered": 1, "closed": 2}
    all_results.sort(key=lambda r: (state_order.get(r["state"], 9), r["host"], r["port"]))

    csv_rows = [r for r in all_results if r["state"] == "open"] if args.open_only else all_results
    write_results(csv_rows, args.output)
    print(f"\nResults written to: {args.output}  ({len(csv_rows)} rows)")

    print_summary(all_results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
