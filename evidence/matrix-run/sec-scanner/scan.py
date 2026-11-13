#!/usr/bin/env python3
"""
scan.py — threaded TCP connect scanner
Python 3.12, stdlib only

Usage examples:
    python scan.py --cidr 10.0.0.0/24
    python scan.py --cidr 192.168.1.1 192.168.1.50 --ports 22,80,443,8000-8090
    python scan.py --hosts-file targets.txt --threads 200 --timeout 0.5
    python scan.py --cidr 10.0.1.0/28 --no-banner --open-only --output found.csv
"""

import argparse
import csv
import ipaddress
import socket
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Common ports — keyed by port number, value is a human-readable service hint
# ---------------------------------------------------------------------------
COMMON_PORTS: dict[int, str] = {
    21:    "ftp",
    22:    "ssh",
    23:    "telnet",
    25:    "smtp",
    53:    "dns",
    80:    "http",
    110:   "pop3",
    111:   "rpcbind",
    135:   "msrpc",
    139:   "netbios-ssn",
    143:   "imap",
    443:   "https",
    445:   "smb",
    465:   "smtps",
    587:   "submission",
    631:   "ipp",
    993:   "imaps",
    995:   "pop3s",
    1433:  "mssql",
    1521:  "oracle",
    1723:  "pptp",
    2049:  "nfs",
    2181:  "zookeeper",
    2375:  "docker",
    2376:  "docker-tls",
    3000:  "dev-http",
    3306:  "mysql",
    3389:  "rdp",
    4443:  "alt-https",
    4848:  "glassfish",
    5432:  "postgresql",
    5900:  "vnc",
    5984:  "couchdb",
    6379:  "redis",
    6443:  "k8s-api",
    7001:  "weblogic",
    8080:  "http-alt",
    8443:  "https-alt",
    8888:  "jupyter",
    9000:  "php-fpm",
    9090:  "prometheus",
    9200:  "elasticsearch",
    9300:  "es-cluster",
    11211: "memcached",
    27017: "mongodb",
    27018: "mongodb-shard",
    50000: "db2",
}

# How much to read during a raw banner grab
BANNER_READ_BYTES = 1024
# Separate short timeout used only during banner I/O (not the connect)
BANNER_IO_TIMEOUT = 2.0

CSV_FIELDS = ["timestamp", "host", "port", "service", "state", "banner"]


# ---------------------------------------------------------------------------
# Target expansion
# ---------------------------------------------------------------------------

def expand_targets(raw: list[str]) -> list[str]:
    """
    Accept CIDR blocks, bare IPs, and hostnames; return a flat, deduplicated
    list preserving order.  Lines starting with '#' and blank lines are skipped.
    """
    seen: set[str] = set()
    hosts: list[str] = []

    for entry in raw:
        entry = entry.strip()
        if not entry or entry.startswith("#"):
            continue
        try:
            net = ipaddress.ip_network(entry, strict=False)
            # For a /32 or /128, hosts() is empty — use network_address instead.
            addrs = list(net.hosts()) or [net.network_address]
            for ip in addrs:
                key = str(ip)
                if key not in seen:
                    seen.add(key)
                    hosts.append(key)
        except ValueError:
            # Not a valid CIDR — treat as a plain hostname/IP string.
            if entry not in seen:
                seen.add(entry)
                hosts.append(entry)

    return hosts


# ---------------------------------------------------------------------------
# Port list parsing
# ---------------------------------------------------------------------------

def parse_port_spec(spec: str) -> list[int]:
    """
    Parse a comma-separated port spec that may include ranges, e.g.
    "22,80,443,8000-8090".  Returns a sorted, deduplicated list.
    """
    ports: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_s, hi_s = part.split("-", 1)
            lo, hi = int(lo_s.strip()), int(hi_s.strip())
            if not (1 <= lo <= 65535 and 1 <= hi <= 65535 and lo <= hi):
                raise ValueError(f"bad range: {part!r}")
            ports.update(range(lo, hi + 1))
        else:
            port = int(part)
            if not 1 <= port <= 65535:
                raise ValueError(f"port out of range: {port}")
            ports.add(port)
    return sorted(ports)


# ---------------------------------------------------------------------------
# Banner grabbing
# ---------------------------------------------------------------------------

def _decode(data: bytes) -> str:
    """Decode bytes to a printable, single-line string (max 256 chars)."""
    text = data.decode("latin-1", errors="replace")
    # Collapse whitespace / control chars to spaces, then trim.
    cleaned = " ".join(text.split())
    return cleaned[:256]


def grab_banner(host: str, port: int) -> str:
    """
    1. Open a fresh connection and wait briefly for the server to speak first
       (many protocols — SSH, FTP, SMTP — send a greeting on connect).
    2. If the server says nothing, fall back to an HTTP/1.0 HEAD request so
       we still get a status line + Server header from web services.
    Returns a single sanitised line, or "" on failure.
    """
    # --- raw read ---
    try:
        with socket.create_connection((host, port), timeout=BANNER_IO_TIMEOUT) as s:
            s.settimeout(BANNER_IO_TIMEOUT)
            try:
                data = s.recv(BANNER_READ_BYTES)
                if data:
                    return _decode(data)
            except (socket.timeout, OSError):
                pass
    except OSError:
        pass

    # --- HTTP HEAD fallback ---
    try:
        with socket.create_connection((host, port), timeout=BANNER_IO_TIMEOUT) as s:
            s.settimeout(BANNER_IO_TIMEOUT)
            request = (
                f"HEAD / HTTP/1.0\r\n"
                f"Host: {host}\r\n"
                f"User-Agent: scan/1.0\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            ).encode()
            s.sendall(request)
            # Collect headers only — stop at the blank line or 4 KB.
            buf = b""
            while len(buf) < 4096:
                try:
                    chunk = s.recv(512)
                except (socket.timeout, OSError):
                    break
                if not chunk:
                    break
                buf += chunk
                if b"\r\n\r\n" in buf:
                    buf = buf.split(b"\r\n\r\n", 1)[0]
                    break
            if buf:
                # Return the first response line (e.g. "HTTP/1.1 200 OK").
                first_line = buf.split(b"\r\n", 1)[0]
                return _decode(first_line)
    except OSError:
        pass

    return ""


# ---------------------------------------------------------------------------
# Per-port scan task
# ---------------------------------------------------------------------------

def scan_port(host: str, port: int, timeout: float, grab: bool) -> dict:
    """
    Attempt a TCP connect.  On success, optionally grab a banner.
    Returns a result dict with keys matching CSV_FIELDS.
    States: open | closed | filtered | error:<msg>
    """
    ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    service = COMMON_PORTS.get(port, "")

    base: dict = {
        "timestamp": ts,
        "host":      host,
        "port":      port,
        "service":   service,
        "state":     "closed",
        "banner":    "",
    }

    try:
        with socket.create_connection((host, port), timeout=timeout):
            base["state"] = "open"
    except ConnectionRefusedError:
        base["state"] = "closed"
        return base
    except socket.timeout:
        base["state"] = "filtered"
        return base
    except OSError as exc:
        base["state"] = f"error: {exc}"
        return base

    if grab:
        base["banner"] = grab_banner(host, port)

    return base


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="scan.py",
        description=(
            "Threaded TCP connect scanner.  "
            "Accepts CIDR blocks or a host list file, "
            "grabs banners, and writes per-port results to CSV."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python scan.py --cidr 192.168.1.0/24\n"
            "  python scan.py --cidr 10.0.0.1 --ports 22,80,443\n"
            "  python scan.py --hosts-file targets.txt --threads 250 --timeout 0.5\n"
            "  python scan.py --cidr 10.0.0.0/28 --no-banner --open-only"
        ),
    )

    tg = p.add_mutually_exclusive_group(required=True)
    tg.add_argument(
        "--cidr",
        metavar="NETWORK",
        nargs="+",
        help="One or more CIDR blocks or plain IPs/hostnames",
    )
    tg.add_argument(
        "--hosts-file",
        metavar="FILE",
        help="Text file with one host or CIDR per line (# lines ignored)",
    )

    p.add_argument(
        "--ports",
        metavar="SPEC",
        default=None,
        help=(
            "Comma-separated ports / ranges, e.g. 22,80,443,8000-8080.  "
            "Defaults to the built-in common-port list."
        ),
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=1.0,
        metavar="SECS",
        help="TCP connect timeout per port in seconds (default: 1.0)",
    )
    p.add_argument(
        "--threads",
        type=int,
        default=100,
        metavar="N",
        help="Thread pool size (default: 100)",
    )
    p.add_argument(
        "--no-banner",
        action="store_true",
        help="Skip banner grabbing — faster but no service detail",
    )
    p.add_argument(
        "--open-only",
        action="store_true",
        help="Write only open-port rows to CSV; filtered/closed rows are omitted",
    )
    p.add_argument(
        "--output",
        metavar="FILE",
        default="scan_results.csv",
        help="CSV output path (default: scan_results.csv)",
    )
    return p


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    # --- resolve targets ---
    if args.cidr:
        raw_targets = args.cidr
    else:
        host_path = Path(args.hosts_file)
        if not host_path.is_file():
            parser.error(f"hosts file not found: {args.hosts_file}")
        raw_targets = host_path.read_text(encoding="utf-8").splitlines()

    hosts = expand_targets(raw_targets)
    if not hosts:
        parser.error("no valid targets found in input")

    # --- resolve ports ---
    if args.ports:
        try:
            ports = parse_port_spec(args.ports)
        except ValueError as exc:
            parser.error(f"invalid port spec — {exc}")
    else:
        ports = sorted(COMMON_PORTS)

    grab = not args.no_banner
    total = len(hosts) * len(ports)

    print(
        f"Scanning  {len(hosts)} host(s)  ×  {len(ports)} port(s)  =  {total} tasks\n"
        f"          threads={args.threads}  timeout={args.timeout}s  "
        f"banner={'on' if grab else 'off'}  output={args.output}"
    )
    print()

    # --- dispatch ---
    results: list[dict] = []
    completed = 0

    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        future_map = {
            pool.submit(scan_port, host, port, args.timeout, grab): (host, port)
            for host in hosts
            for port in ports
        }

        for future in as_completed(future_map):
            completed += 1
            if completed % 200 == 0 or completed == total:
                pct = completed / total * 100
                print(f"  progress: {completed}/{total}  ({pct:.1f}%)", end="\r", flush=True)

            try:
                result = future.result()
            except Exception as exc:
                host, port = future_map[future]
                result = {
                    "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                    "host":    host,
                    "port":    port,
                    "service": COMMON_PORTS.get(port, ""),
                    "state":   f"error: {exc}",
                    "banner":  "",
                }
            results.append(result)

    print()  # clear the progress line

    # --- sort by host then port ---
    results.sort(key=lambda r: (r["host"], int(r["port"])))

    # --- write CSV ---
    output_path = Path(args.output)
    rows_written = 0
    with output_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in results:
            if args.open_only and row["state"] != "open":
                continue
            writer.writerow(row)
            rows_written += 1

    print(f"Wrote {rows_written} row(s) to {output_path}")

    # --- summary ---
    open_by_host: dict[str, list[int]] = {}
    for row in results:
        if row["state"] == "open":
            open_by_host.setdefault(row["host"], []).append(int(row["port"]))

    total_open = sum(len(v) for v in open_by_host.values())
    divider = "─" * 62

    print()
    print(divider)
    print(f"  Open ports: {total_open}  across  {len(open_by_host)} host(s)")

    if open_by_host:
        print()
        for host in sorted(open_by_host, key=lambda h: (
            # Sort numerically when it looks like an IP, lexically otherwise.
            [int(x) for x in h.split(".")] if h.replace(".", "").isdigit() else [h]
        )):
            port_list = ", ".join(
                f"{p}/{COMMON_PORTS.get(p, '?')}"
                for p in sorted(open_by_host[host])
            )
            print(f"  {host:<22} {port_list}")

    print(divider)


if __name__ == "__main__":
    main()
