#!/usr/bin/env python3
"""scan.py — threaded TCP connect scanner.

Scans a CIDR range or a list of hosts against a set of TCP ports, grabs
service banners (short read with an HTTP HEAD fallback), writes per-port
result rows to CSV, and prints a summary of open counts.

Python 3.12, standard library only.

Examples
--------
# Scan a /24 over the default common ports, 50 worker threads
python scan.py --cidr 192.168.1.0/24 --threads 50 -o results.csv

# Scan a host list file over a custom port set
python scan.py --hosts hosts.txt --ports 22,80,443,8080 -o results.csv

# Scan inline hosts
python scan.py --hosts 10.0.0.5,example.com --ports 1-1024 -o results.csv
"""

import argparse
import csv
import ipaddress
import socket
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

# A useful default set of common TCP ports. Not exhaustive, but covers the
# services people actually look for during a quick sweep.
DEFAULT_PORTS = [
    20, 21, 22, 23, 25, 53, 69, 80, 110, 111, 119, 123, 135, 137, 138, 139,
    143, 161, 162, 389, 443, 445, 465, 514, 587, 631, 636, 873, 990, 993,
    995, 1080, 1194, 1433, 1521, 1723, 1900, 2049, 2121, 2222, 2375, 2376,
    3128, 3268, 3306, 3389, 4369, 5000, 5060, 5432, 5601, 5672, 5900, 5984,
    6379, 6443, 6660, 6666, 6667, 6668, 6669, 7000, 7001, 8000, 8008, 8009,
    8080, 8081, 8086, 8088, 8443, 8888, 9000, 9042, 9090, 9092, 9100, 9200,
    9300, 9418, 10000, 11211, 15672, 25565, 27015, 27017, 50070,
]

# How long to wait for banner bytes after a connect succeeds. Kept short so a
# silent service does not stall a worker thread.
BANNER_READ_TIMEOUT = 2.0
# Bytes to read from an initial banner push.
BANNER_READ_SIZE = 1024
# Bytes to read back after sending an HTTP HEAD probe.
HEAD_READ_SIZE = 4096


def expand_cidr(cidr: str) -> list[str]:
    """Expand a CIDR block into individual host address strings.

    Network and broadcast addresses are skipped for prefixes shorter than
    /31. /32 (single host) is returned as-is.
    """
    try:
        net = ipaddress.ip_network(cidr, strict=False)
    except ValueError as exc:
        raise SystemExit(f"scan.py: invalid CIDR {cidr!r}: {exc}") from exc
    if net.prefixlen == 32:
        return [str(net.network_address)]
    if net.prefixlen == 31:
        return [str(h) for h in net.hosts()]
    # hosts() already excludes network/broadcast for normal prefixes.
    return [str(h) for h in net.hosts()]


def load_hosts(source: str) -> list[str]:
    """Resolve a host input into a list of target address strings.

    Accepts a comma-separated list ("a,b,c"), a filename when the value ends
    with .txt/.lst/.csv or names an existing readable file, or a bare hostname.
    Hostnames are resolved to addresses at scan time, so they are returned as
    written here.
    """
    import os

    hosts: list[str] = []
    # A bare comma list is the most common inline form.
    if "," in source:
        for part in source.split(","):
            part = part.strip()
            if part:
                hosts.append(part)
        return hosts
    # File path: detect by extension or by the file actually existing.
    if source.endswith((".txt", ".lst", ".csv")) or (
        os.path.isfile(source) and os.access(source, os.R_OK)
    ):
        try:
            with open(source, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        hosts.append(line)
        except OSError as exc:
            raise SystemExit(f"scan.py: cannot read hosts file {source!r}: {exc}") from exc
        if not hosts:
            raise SystemExit(f"scan.py: no hosts found in {source!r}")
        return hosts
    # Single bare host.
    return [source]


def parse_ports(spec: str) -> list[int]:
    """Parse a port spec into a sorted, de-duplicated list of ints.

    Accepts comma-separated values and ranges ("22,80,1000-1024"). An empty
    spec yields DEFAULT_PORTS.
    """
    if not spec:
        return sorted(DEFAULT_PORTS)
    ports: set[int] = set()
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            lo, _, hi = token.partition("-")
            try:
                lo_i, hi_i = int(lo), int(hi)
            except ValueError as exc:
                raise SystemExit(f"scan.py: bad port range {token!r}: {exc}") from exc
            if lo_i < 1 or hi_i > 65535 or lo_i > hi_i:
                raise SystemExit(f"scan.py: invalid port range {token!r}")
            ports.update(range(lo_i, hi_i + 1))
        else:
            try:
                p = int(token)
            except ValueError as exc:
                raise SystemExit(f"scan.py: bad port {token!r}: {exc}") from exc
            if p < 1 or p > 65535:
                raise SystemExit(f"scan.py: port {p} out of range 1-65535")
            ports.add(p)
    if not ports:
        return sorted(DEFAULT_PORTS)
    return sorted(ports)


def resolve_host(host: str) -> list[str]:
    """Resolve a host (name or literal) to connectable address strings.

    A literal IP is returned unchanged. A hostname is resolved to one or more
    addresses; resolution failure returns an empty list so the caller can mark
    the host as unreachable rather than crashing the scan.
    """
    # Literal IPv4/IPv6 needs no DNS.
    try:
        ipaddress.ip_address(host)
        return [host]
    except ValueError:
        pass
    try:
        # family=0 lets getaddrinfo pick both v4 and v6.
        infos = socket.getaddrinfo(host, None, proto=socket.IPPROTO_TCP)
    except socket.gaierror:
        return []
    # De-duplicate while preserving order.
    seen: set[str] = set()
    addrs: list[str] = []
    for info in infos:
        sockaddr = info[4]
        ip = sockaddr[0]
        if ip not in seen:
            seen.add(ip)
            addrs.append(ip)
    return addrs


def grab_banner(sock: socket.socket, host: str, port: int, timeout: float) -> str:
    """Best-effort banner grab from an already-connected socket.

    First tries a short read: many services greet on connect (SSH, FTP, SMTP,
    SSH, Redis, etc.). If nothing arrives within a short window, sends an HTTP
    HEAD request and reads the response. Returns the decoded banner string,
    stripped of trailing whitespace. Unknown/decode errors fall back to a
    latin-1 decode so binary banners still surface.
    """
    banner = ""
    sock.settimeout(min(timeout, BANNER_READ_TIMEOUT))
    # Step 1: short read for services that greet immediately.
    try:
        chunk = sock.recv(BANNER_READ_SIZE)
        if chunk:
            banner = chunk.decode("utf-8", errors="replace")
    except socket.timeout:
        pass
    except OSError:
        pass

    if banner.strip():
        return banner.strip()

    # Step 2: HTTP HEAD fallback for HTTP-like services.
    head = (
        f"HEAD / HTTP/1.0\r\n"
        f"Host: {host}\r\n"
        f"User-Agent: scan.py\r\n"
        f"Connection: close\r\n\r\n"
    ).encode("ascii", errors="ignore")
    try:
        sock.sendall(head)
        sock.settimeout(min(timeout, BANNER_READ_TIMEOUT))
        chunk = sock.recv(HEAD_READ_SIZE)
        if chunk:
            banner = chunk.decode("utf-8", errors="replace")
    except OSError:
        pass

    return banner.strip()


def scan_port(host: str, port: int, timeout: float) -> dict:
    """Attempt a single TCP connect against host:port.

    Returns a result dict with keys: host, port, state, banner. States are
    'open', 'closed', or 'filtered' (filtered = connect timed out or was
    refused in a way that suggests a dropped packet, versus a clean RST).
    """
    result = {"host": host, "port": port, "state": "filtered", "banner": ""}
    # Prefer IPv6-aware connect when the resolved address is v6.
    try:
        # getaddrinfo picks the right family for the literal/hostname we pass.
        infos = socket.getaddrinfo(host, port, proto=socket.IPPROTO_TCP)
    except socket.gaierror:
        result["state"] = "closed"
        return result

    last_exc = None
    for family, stype, proto, _canon, sockaddr in infos:
        sock = None
        try:
            sock = socket.socket(family, stype, proto)
            sock.settimeout(timeout)
            sock.connect(sockaddr)
            # Connected: the port is open.
            result["state"] = "open"
            result["banner"] = grab_banner(sock, host, port, timeout)
            return result
        except ConnectionRefusedError:
            # Clean RST -> definitively closed.
            result["state"] = "closed"
            last_exc = "refused"
        except socket.timeout:
            # Dropped packets -> filtered.
            result["state"] = "filtered"
            last_exc = "timeout"
        except OSError as exc:
            # Other errors (no route, host unreachable, etc.) -> filtered.
            result["state"] = "filtered"
            last_exc = str(exc)
        finally:
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
    # Return the last state observed across address attempts.
    return result


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="scan.py",
        description="Threaded TCP connect scanner with banner grabbing.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--cidr",
        metavar="CIDR",
        help="target network in CIDR notation, e.g. 192.168.1.0/24",
    )
    src.add_argument(
        "--hosts",
        metavar="HOSTS",
        help="comma-separated host list or path to a host file",
    )
    parser.add_argument(
        "-p", "--ports",
        metavar="SPEC",
        default="",
        help="ports: comma list and ranges, e.g. '22,80,443,1000-2000' "
             "(default: built-in common port set)",
    )
    parser.add_argument(
        "-t", "--timeout",
        type=float,
        default=2.0,
        metavar="SECS",
        help="per-connection connect timeout in seconds (default: 2.0)",
    )
    parser.add_argument(
        "-T", "--threads",
        type=int,
        default=100,
        metavar="N",
        help="thread pool size (default: 100)",
    )
    parser.add_argument(
        "-o", "--output",
        default="scan_results.csv",
        metavar="FILE",
        help="output CSV path (default: scan_results.csv)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)

    # Build the target host list.
    if args.cidr:
        targets = expand_cidr(args.cidr)
    else:
        targets = load_hosts(args.hosts)

    ports = parse_ports(args.ports)

    if args.threads < 1:
        raise SystemExit("scan.py: --threads must be >= 1")
    if args.timeout <= 0:
        raise SystemExit("scan.py: --timeout must be > 0")

    total_checks = len(targets) * len(ports)
    print(
        f"scan.py: {len(targets)} host(s) x {len(ports)} port(s) = "
        f"{total_checks} checks, {args.threads} threads, "
        f"{args.timeout}s timeout",
        file=sys.stderr,
    )

    rows: list[dict] = []
    done = 0

    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        future_map = {}
        for host in targets:
            # Resolve once up front so each host's addresses are reused, and so
            # resolution failures surface as closed hosts rather than crashes.
            resolved = resolve_host(host)
            if not resolved:
                # Emit a row per port marking the host unresolvable.
                for port in ports:
                    rows.append({
                        "host": host,
                        "port": port,
                        "state": "closed",
                        "banner": "unresolvable host",
                    })
                    done += 1
                continue
            for port in ports:
                # Pass the original host string so banner HEAD uses a clean name.
                fut = pool.submit(scan_port, host, port, args.timeout)
                future_map[fut] = (host, port)

        for fut in as_completed(future_map):
            host, port = future_map[fut]
            try:
                row = fut.result()
            except Exception as exc:  # noqa: BLE001 - keep scanning on any error
                row = {"host": host, "port": port, "state": "filtered",
                       "banner": f"error: {exc}"}
            rows.append(row)
            done += 1
            if done % 100 == 0 or done == total_checks:
                print(f"  ...{done}/{total_checks} done", file=sys.stderr, end="\r")

    print(file=sys.stderr)

    # Sort for stable output: by host (numeric where possible), then port.
    def sort_key(row: dict):
        try:
            ip = ipaddress.ip_address(row["host"])
            return (0, int(ip), int(row["port"]))
        except ValueError:
            return (1, row["host"], int(row["port"]))

    rows.sort(key=sort_key)

    # Write CSV.
    fieldnames = ["host", "port", "state", "banner"]
    try:
        with open(args.output, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)
    except OSError as exc:
        raise SystemExit(f"scan.py: cannot write {args.output!r}: {exc}") from exc

    # Summary: open counts per host and per port.
    open_rows = [r for r in rows if r["state"] == "open"]
    open_by_host: dict[str, int] = {}
    open_by_port: dict[int, int] = {}
    for r in open_rows:
        open_by_host[r["host"]] = open_by_host.get(r["host"], 0) + 1
        open_by_port[r["port"]] = open_by_port.get(r["port"], 0) + 1

    state_counts: dict[str, int] = {}
    for r in rows:
        state_counts[r["state"]] = state_counts.get(r["state"], 0) + 1

    print("\n=== Scan summary ===")
    print(f"Hosts scanned : {len(targets)}")
    print(f"Ports probed  : {len(ports)}")
    print(f"Total checks  : {total_checks}")
    for state in ("open", "closed", "filtered"):
        print(f"  {state:<9}: {state_counts.get(state, 0)}")
    print(f"Open ports    : {len(open_rows)}")

    if open_by_host:
        print("\nOpen ports per host:")
        for host in sorted(open_by_host, key=_host_sort_key):
            print(f"  {host}: {open_by_host[host]} open")

    if open_by_port:
        print("\nTop open ports:")
        for port, count in sorted(open_by_port.items(),
                                   key=lambda kv: kv[1], reverse=True)[:15]:
            try:
                service = socket.getservbyport(port, "tcp")
            except OSError:
                service = "?"
            print(f"  {port:<6} ({service:<12}) open on {count} host(s)")

    print(f"\nResults written to {args.output}")
    return 0


def _host_sort_key(host: str):
    try:
        return (0, int(ipaddress.ip_address(host)))
    except ValueError:
        return (1, host)


if __name__ == "__main__":
    raise SystemExit(main())
