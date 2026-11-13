#!/usr/bin/env python3
"""
scan.py — Threaded TCP connect scanner
Python 3.12, stdlib only

Usage:
  python scan.py --targets 192.168.1.0/24 10.0.0.5 --ports 22 80 443 --threads 100
  python scan.py --target-file hosts.txt --timeout 1.5 --output results.csv
"""

import argparse
import csv
import ipaddress
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, fields
from datetime import datetime, timezone
from typing import Iterable

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

COMMON_PORTS: list[int] = [
    21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445,
    465, 587, 993, 995, 1433, 1521, 3306, 3389, 5432, 5900, 6379,
    8080, 8443, 8888, 9200, 9300, 27017,
]

DEFAULT_TIMEOUT: float = 1.0
DEFAULT_THREADS: int = 200
BANNER_READ_BYTES: int = 1024
BANNER_TIMEOUT: float = 2.0
HTTP_HEAD_TIMEOUT: float = 3.0


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

@dataclass
class ScanResult:
    host: str
    port: int
    state: str          # "open" | "closed" | "filtered"
    banner: str
    http_status: str    # HTTP status line, or empty
    latency_ms: float
    scanned_at: str


CSV_FIELDS = [f.name for f in fields(ScanResult)]


# ---------------------------------------------------------------------------
# Network helpers
# ---------------------------------------------------------------------------

def _tcp_connect(host: str, port: int, timeout: float) -> tuple[bool, float, socket.socket | None]:
    """Return (connected, latency_ms, socket_or_None)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    t0 = time.perf_counter()
    try:
        sock.connect((host, port))
        latency = (time.perf_counter() - t0) * 1000
        return True, latency, sock
    except (socket.timeout, ConnectionRefusedError, OSError):
        latency = (time.perf_counter() - t0) * 1000
        sock.close()
        return False, latency, None


def _grab_banner(sock: socket.socket) -> str:
    """Try a short read to get a service banner."""
    try:
        sock.settimeout(BANNER_TIMEOUT)
        data = sock.recv(BANNER_READ_BYTES)
        return data.decode("utf-8", errors="replace").strip()
    except (socket.timeout, OSError):
        return ""


def _http_head(host: str, port: int) -> str:
    """
    Send an HTTP HEAD / request and return the status line.
    Falls back to empty string on any error.
    """
    try:
        sock = socket.create_connection((host, port), timeout=HTTP_HEAD_TIMEOUT)
        request = f"HEAD / HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n"
        sock.sendall(request.encode())
        sock.settimeout(HTTP_HEAD_TIMEOUT)
        response = b""
        while True:
            chunk = sock.recv(256)
            if not chunk:
                break
            response += chunk
            if b"\r\n" in response:
                break
        sock.close()
        first_line = response.split(b"\r\n", 1)[0]
        return first_line.decode("utf-8", errors="replace").strip()
    except (socket.timeout, OSError, UnicodeDecodeError):
        return ""


# ---------------------------------------------------------------------------
# Per-port scan logic
# ---------------------------------------------------------------------------

def scan_port(host: str, port: int, timeout: float) -> ScanResult:
    ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    connected, latency_ms, sock = _tcp_connect(host, port, timeout)

    if not connected:
        return ScanResult(
            host=host,
            port=port,
            state="closed/filtered",
            banner="",
            http_status="",
            latency_ms=round(latency_ms, 2),
            scanned_at=ts,
        )

    # Port is open — grab banner first
    banner = _grab_banner(sock)
    try:
        sock.close()
    except OSError:
        pass

    # HTTP HEAD fallback when banner is empty and port looks HTTP-ish
    http_status = ""
    if not banner or port in {80, 8080, 8888, 443, 8443}:
        http_status = _http_head(host, port)

    return ScanResult(
        host=host,
        port=port,
        state="open",
        banner=banner,
        http_status=http_status,
        latency_ms=round(latency_ms, 2),
        scanned_at=ts,
    )


# ---------------------------------------------------------------------------
# Target expansion
# ---------------------------------------------------------------------------

def expand_targets(raw: Iterable[str]) -> list[str]:
    """Expand CIDRs and plain hostnames/IPs into a flat list of strings."""
    out: list[str] = []
    for item in raw:
        item = item.strip()
        if not item or item.startswith("#"):
            continue
        try:
            network = ipaddress.ip_network(item, strict=False)
            out.extend(str(ip) for ip in network.hosts())
        except ValueError:
            # Treat as a hostname or bare IP
            out.append(item)
    return out


def load_target_file(path: str) -> list[str]:
    with open(path, encoding="utf-8") as fh:
        return list(fh)


# ---------------------------------------------------------------------------
# Scan orchestration
# ---------------------------------------------------------------------------

def run_scan(
    targets: list[str],
    ports: list[int],
    timeout: float,
    threads: int,
    output: str,
) -> None:
    total_jobs = len(targets) * len(ports)
    if total_jobs == 0:
        print("No targets or ports to scan.", file=sys.stderr)
        sys.exit(1)

    print(
        f"[*] Scanning {len(targets)} host(s) × {len(ports)} port(s) = {total_jobs} jobs "
        f"| threads={threads} | timeout={timeout}s | output={output}"
    )

    open_counts: dict[str, int] = {}  # host -> open port count
    results: list[ScanResult] = []
    done = 0

    with open(output, "w", newline="", encoding="utf-8") as csv_fh:
        writer = csv.DictWriter(csv_fh, fieldnames=CSV_FIELDS)
        writer.writeheader()

        with ThreadPoolExecutor(max_workers=threads) as pool:
            future_map = {
                pool.submit(scan_port, host, port, timeout): (host, port)
                for host in targets
                for port in ports
            }

            for future in as_completed(future_map):
                result = future.result()
                results.append(result)
                writer.writerow(
                    {f: getattr(result, f) for f in CSV_FIELDS}
                )

                done += 1
                if result.state == "open":
                    open_counts[result.host] = open_counts.get(result.host, 0) + 1
                    print(
                        f"  OPEN  {result.host}:{result.port}"
                        + (f"  [{result.banner[:60]}]" if result.banner else "")
                        + (f"  [{result.http_status}]" if result.http_status else "")
                    )

                # Progress tick every 500 jobs
                if done % 500 == 0 or done == total_jobs:
                    pct = done / total_jobs * 100
                    print(f"  ... {done}/{total_jobs} ({pct:.1f}%)", end="\r", flush=True)

    print()  # clear the \r line

    # Summary
    total_open = sum(open_counts.values())
    print(f"\n[+] Scan complete — {total_open} open port(s) across {len(open_counts)} host(s)")
    print(f"[+] Results written to: {output}\n")

    if open_counts:
        print("Open port counts per host:")
        for host, count in sorted(open_counts.items(), key=lambda kv: kv[1], reverse=True):
            print(f"  {host}: {count}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Threaded TCP connect scanner — CIDR/host-list input, banner grab, CSV output",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    target_group = parser.add_mutually_exclusive_group(required=True)
    target_group.add_argument(
        "--targets", nargs="+", metavar="TARGET",
        help="One or more hosts, IPs, or CIDRs (e.g. 10.0.0.0/24 192.168.1.5)",
    )
    target_group.add_argument(
        "--target-file", metavar="FILE",
        help="Path to a newline-separated list of hosts/CIDRs",
    )

    parser.add_argument(
        "--ports", nargs="+", type=int, metavar="PORT",
        default=None,
        help=(
            "Ports to scan (default: built-in common-port list of "
            f"{len(COMMON_PORTS)} ports)"
        ),
    )
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT, metavar="SECONDS",
        help=f"TCP connect timeout in seconds (default: {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--threads", type=int, default=DEFAULT_THREADS, metavar="N",
        help=f"Thread pool size (default: {DEFAULT_THREADS})",
    )
    parser.add_argument(
        "--output", default="scan_results.csv", metavar="CSV",
        help="Output CSV file path (default: scan_results.csv)",
    )

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    # Resolve targets
    if args.target_file:
        raw_targets = load_target_file(args.target_file)
    else:
        raw_targets = args.targets

    targets = expand_targets(raw_targets)
    if not targets:
        parser.error("No valid targets found.")

    ports = args.ports if args.ports else COMMON_PORTS

    run_scan(
        targets=targets,
        ports=ports,
        timeout=args.timeout,
        threads=args.threads,
        output=args.output,
    )


if __name__ == "__main__":
    main()
