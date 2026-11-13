#!/usr/bin/env python3
"""
scan.py — Threaded TCP connect scanner
Python 3.12, stdlib only

Usage:
    python scan.py --targets 192.168.1.0/24 --ports common --threads 100 --timeout 1.0 --output results.csv
    python scan.py --targets hosts.txt --ports 22,80,443,8080 --threads 50 --timeout 2.0 --output results.csv
"""

import argparse
import csv
import ipaddress
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, fields
from pathlib import Path

# ---------------------------------------------------------------------------
# Common ports (service name, port number)
# ---------------------------------------------------------------------------
COMMON_PORTS: list[int] = [
    21, 22, 23, 25, 53, 80, 110, 111, 119, 135, 139, 143, 161, 194,
    389, 443, 445, 465, 587, 631, 993, 995, 1080, 1194, 1433, 1521,
    1723, 2049, 2181, 3306, 3389, 4444, 5432, 5672, 5900, 5985, 6379,
    6443, 7001, 7443, 8080, 8443, 8888, 9000, 9090, 9200, 9300, 9418,
    27017, 27018, 50070,
]

PORT_SERVICES: dict[int, str] = {
    21: "ftp", 22: "ssh", 23: "telnet", 25: "smtp", 53: "dns",
    80: "http", 110: "pop3", 111: "rpcbind", 119: "nntp", 135: "msrpc",
    139: "netbios", 143: "imap", 161: "snmp", 194: "irc", 389: "ldap",
    443: "https", 445: "smb", 465: "smtps", 587: "submission",
    631: "ipp", 993: "imaps", 995: "pop3s", 1080: "socks",
    1194: "openvpn", 1433: "mssql", 1521: "oracle", 1723: "pptp",
    2049: "nfs", 2181: "zookeeper", 3306: "mysql", 3389: "rdp",
    4444: "metasploit", 5432: "postgres", 5672: "amqp", 5900: "vnc",
    5985: "winrm", 6379: "redis", 6443: "k8s-api", 7001: "weblogic",
    7443: "alt-https", 8080: "http-alt", 8443: "https-alt",
    8888: "jupyter", 9000: "php-fpm", 9090: "prometheus",
    9200: "elasticsearch", 9300: "es-transport", 9418: "git",
    27017: "mongodb", 27018: "mongodb-alt", 50070: "hdfs",
}

# Short HTTP HEAD request used as a fallback banner probe
_HTTP_HEAD = b"HEAD / HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n"
# Max bytes to read from banner grab / HTTP response
BANNER_READ_BYTES = 1024
# Seconds to wait for banner data after connect
BANNER_TIMEOUT = 2.0


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------
@dataclass
class ScanResult:
    host: str
    port: int
    service: str
    state: str          # "open" | "closed" | "filtered"
    banner: str
    latency_ms: float


CSV_FIELDS = [f.name for f in fields(ScanResult)]


# ---------------------------------------------------------------------------
# Target expansion
# ---------------------------------------------------------------------------
def expand_targets(spec: str) -> list[str]:
    """
    Accept a CIDR block, a single host/IP, or a path to a file containing
    one entry per line (each of which may itself be a CIDR or host).
    Returns a flat list of string targets.
    """
    p = Path(spec)
    if p.exists() and p.is_file():
        lines = p.read_text().splitlines()
        targets: list[str] = []
        for line in lines:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            targets.extend(_expand_one(line))
        return targets
    return _expand_one(spec)


def _expand_one(spec: str) -> list[str]:
    try:
        network = ipaddress.ip_network(spec, strict=False)
        # For a /32 or single host, ip_network still works fine
        return [str(h) for h in network.hosts()] or [str(network.network_address)]
    except ValueError:
        # Treat as a hostname
        return [spec]


# ---------------------------------------------------------------------------
# Port parsing
# ---------------------------------------------------------------------------
def parse_ports(spec: str) -> list[int]:
    """
    "common"           → COMMON_PORTS
    "80,443,8080"      → [80, 443, 8080]
    "1-1024"           → list(range(1, 1025))
    "22,80-85,443"     → mixed
    """
    if spec.strip().lower() == "common":
        return COMMON_PORTS

    ports: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            lo, hi = part.split("-", 1)
            ports.extend(range(int(lo), int(hi) + 1))
        else:
            ports.append(int(part))
    # Deduplicate, preserve order
    seen: set[int] = set()
    unique: list[int] = []
    for p in ports:
        if p not in seen:
            seen.add(p)
            unique.append(p)
    return unique


# ---------------------------------------------------------------------------
# Banner grab
# ---------------------------------------------------------------------------
def _grab_banner(sock: socket.socket, host: str, port: int) -> str:
    """
    Try a raw read first (catches FTP, SSH, SMTP welcome banners).
    If that returns nothing, send an HTTP HEAD and read the response line.
    Returns a sanitised single-line string.
    """
    sock.settimeout(BANNER_TIMEOUT)
    banner = b""
    try:
        banner = sock.recv(BANNER_READ_BYTES)
    except (socket.timeout, OSError):
        pass

    if not banner:
        try:
            sock.sendall(_HTTP_HEAD.replace(b"{host}", host.encode()))
            banner = sock.recv(BANNER_READ_BYTES)
        except (OSError, socket.timeout):
            pass

    if not banner:
        return ""

    # Keep only the first non-empty line, decode safely
    text = banner.decode("utf-8", errors="replace")
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line[:256]
    return ""


# ---------------------------------------------------------------------------
# Single port probe
# ---------------------------------------------------------------------------
def probe(host: str, port: int, timeout: float) -> ScanResult:
    service = PORT_SERVICES.get(port, "")
    t0 = time.monotonic()

    try:
        sock = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET,
                             socket.SOCK_STREAM)
        sock.settimeout(timeout)
        err = sock.connect_ex((host, port))
        latency_ms = (time.monotonic() - t0) * 1000

        if err == 0:
            banner = _grab_banner(sock, host, port)
            sock.close()
            return ScanResult(host, port, service, "open", banner, round(latency_ms, 2))

        sock.close()
        # ECONNREFUSED → closed; anything else (ETIMEDOUT etc.) → filtered
        import errno
        state = "closed" if err == errno.ECONNREFUSED else "filtered"
        return ScanResult(host, port, service, state, "", round(latency_ms, 2))

    except OSError as exc:
        latency_ms = (time.monotonic() - t0) * 1000
        return ScanResult(host, port, service, "filtered", str(exc)[:80],
                          round(latency_ms, 2))


# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------
def run_scan(
    targets: list[str],
    ports: list[int],
    timeout: float,
    threads: int,
    output_path: str,
    quiet: bool = False,
) -> None:
    total_jobs = len(targets) * len(ports)
    if not quiet:
        print(f"[*] Targets: {len(targets)}  Ports: {len(ports)}  "
              f"Jobs: {total_jobs}  Threads: {threads}  Timeout: {timeout}s",
              file=sys.stderr)

    open_counts: dict[int, int] = {}
    completed = 0

    with open(output_path, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()

        with ThreadPoolExecutor(max_workers=threads) as pool:
            futures = {
                pool.submit(probe, host, port, timeout): (host, port)
                for host in targets
                for port in ports
            }

            for future in as_completed(futures):
                result: ScanResult = future.result()
                completed += 1

                writer.writerow({
                    "host": result.host,
                    "port": result.port,
                    "service": result.service,
                    "state": result.state,
                    "banner": result.banner,
                    "latency_ms": result.latency_ms,
                })
                # Flush periodically so the CSV is usable during a long scan
                if completed % 500 == 0:
                    fh.flush()

                if result.state == "open":
                    open_counts[result.port] = open_counts.get(result.port, 0) + 1
                    if not quiet:
                        svc = f" ({result.service})" if result.service else ""
                        banner = f"  |  {result.banner}" if result.banner else ""
                        print(f"  OPEN  {result.host}:{result.port}{svc}{banner}",
                              file=sys.stderr)

    # Summary
    print(f"\n[+] Scan complete — {completed} probes, results in {output_path}",
          file=sys.stderr)
    if open_counts:
        print("[+] Open port counts:", file=sys.stderr)
        for port, count in sorted(open_counts.items(), key=lambda kv: -kv[1]):
            svc = PORT_SERVICES.get(port, "")
            label = f"{port}/{svc}" if svc else str(port)
            print(f"    {label:30s}  {count}", file=sys.stderr)
    else:
        print("[+] No open ports found.", file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Threaded TCP connect scanner — CIDR/host-list to CSV",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python scan.py --targets 10.0.0.0/24 --ports common --output out.csv
  python scan.py --targets hosts.txt   --ports 22,80,443 --threads 200 --timeout 0.5 --output out.csv
  python scan.py --targets 192.168.1.1 --ports 1-65535   --threads 500 --timeout 1.0 --output out.csv
        """,
    )
    p.add_argument(
        "--targets", required=True,
        help="CIDR (192.168.1.0/24), single host, or path to a file of hosts/CIDRs",
    )
    p.add_argument(
        "--ports", default="common",
        help='Ports to scan: "common", "80,443,8080", or "1-1024" (default: common)',
    )
    p.add_argument(
        "--threads", type=int, default=100,
        help="Thread pool size (default: 100)",
    )
    p.add_argument(
        "--timeout", type=float, default=1.0,
        help="TCP connect timeout in seconds (default: 1.0)",
    )
    p.add_argument(
        "--output", default="scan_results.csv",
        help="CSV output file (default: scan_results.csv)",
    )
    p.add_argument(
        "--quiet", action="store_true",
        help="Suppress per-open-port output; still prints summary",
    )
    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    targets = expand_targets(args.targets)
    if not targets:
        parser.error("No targets found — check --targets value or file.")

    try:
        ports = parse_ports(args.ports)
    except ValueError as exc:
        parser.error(f"Invalid --ports value: {exc}")

    if not ports:
        parser.error("Port list is empty after parsing.")

    run_scan(
        targets=targets,
        ports=ports,
        timeout=args.timeout,
        threads=args.threads,
        output_path=args.output,
        quiet=args.quiet,
    )


if __name__ == "__main__":
    main()
