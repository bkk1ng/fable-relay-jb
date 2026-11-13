#!/usr/bin/env python3
"""
dedupe.py — streaming JSON Lines deduplicator with bounded-memory strategy.

Dedup strategy
--------------
Seen-set:  in-memory Python set of 8-byte BLAKE2b digests (~40 bytes/entry
           with set overhead).  Cheap membership checks, O(1) average.
Spill:     when |mem_set| reaches --spill-at N, the whole set is flushed to
           a temporary SQLite file (WITHOUT ROWID on a BLOB PK, WAL mode) and
           mem_set is cleared.  All subsequent lookups hit memory first, then
           fall through to SQLite only on a miss.
Dupe map:  in-memory {digest -> (truncated_key_repr, count)} populated only
           on the first duplicate hit, so its cardinality is bounded by the
           number of distinct keys that appear more than once.

Usage
-----
  python dedupe.py [OPTIONS] [INPUT]

  INPUT              JSON Lines file; omit or use - for stdin.
  --key FIELD ...    One or more JSON fields that form the composite key.
                     Default: all fields in the object, sorted by name.
  --spill-at N       Flush in-memory digest set to disk when it hits N
                     entries.  Default: 1_000_000  (~40 MB resident).
  --output FILE      Destination for deduplicated output.  Default: stdout.
  --top-dupes N      How many highest-frequency duplicate keys to list in the
                     stats summary.  Default: 20.  Pass 0 to suppress.

Exit codes
----------
  0  success (even if some lines were skipped due to parse errors)
  1  unrecoverable error (I/O failure, bad arguments)
"""

import argparse
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
import time
from typing import Optional


# ---------------------------------------------------------------------------
# Key extraction and digest
# ---------------------------------------------------------------------------

_KEY_REPR_MAX = 200  # bytes stored in dupe map for display


def key_bytes(record: dict, fields: Optional[list[str]]) -> bytes:
    """Return a stable serialisation of the composite key."""
    if fields:
        subset = {f: record.get(f) for f in fields}
    else:
        subset = dict(sorted(record.items()))
    return json.dumps(subset, sort_keys=True, separators=(",", ":")).encode()


def digest8(kb: bytes) -> bytes:
    """8-byte BLAKE2b digest — 2^64 collision resistance for dedup."""
    return hashlib.blake2b(kb, digest_size=8).digest()


# ---------------------------------------------------------------------------
# On-disk seen-set (SQLite)
# ---------------------------------------------------------------------------

class DiskSet:
    """
    Append-only set of 8-byte digests stored in a temporary SQLite database.

    WITHOUT ROWID with a BLOB primary key packs rows tightly and keeps the
    B-tree ordered by the raw 8-byte value, giving fast point lookups even
    after many spills.
    """

    _BATCH = 50_000  # rows per INSERT batch

    def __init__(self, path: str) -> None:
        self._path = path
        self._con = sqlite3.connect(path, isolation_level=None)  # autocommit
        self._con.execute("PRAGMA journal_mode=WAL")
        self._con.execute("PRAGMA synchronous=NORMAL")
        self._con.execute("PRAGMA cache_size=-32768")  # 32 MB page cache
        self._con.execute(
            "CREATE TABLE IF NOT EXISTS seen "
            "(d BLOB NOT NULL PRIMARY KEY) WITHOUT ROWID"
        )

    # -- write ----------------------------------------------------------------

    def bulk_add(self, digests: set[bytes]) -> None:
        """Insert every digest in *digests*, ignoring conflicts."""
        items = list(digests)
        with self._con:
            for start in range(0, len(items), self._BATCH):
                self._con.executemany(
                    "INSERT OR IGNORE INTO seen VALUES (?)",
                    ((d,) for d in items[start : start + self._BATCH]),
                )

    # -- read -----------------------------------------------------------------

    def __contains__(self, d: bytes) -> bool:
        row = self._con.execute(
            "SELECT 1 FROM seen WHERE d=? LIMIT 1", (d,)
        ).fetchone()
        return row is not None

    # -- lifecycle ------------------------------------------------------------

    def close(self) -> None:
        try:
            self._con.close()
        except Exception:
            pass

    def cleanup(self) -> None:
        """Close and delete the backing file."""
        self.close()
        try:
            os.unlink(self._path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Core ETL
# ---------------------------------------------------------------------------

def run(
    in_stream,
    out_stream,
    fields: Optional[list[str]],
    spill_at: int,
    top_dupes: int,
) -> None:
    mem_set: set[bytes] = set()
    disk: Optional[DiskSet] = None
    tmp_path: Optional[str] = None
    spill_count = 0

    # Populated only when a duplicate is encountered for the first time.
    # {digest: (key_repr_truncated, dupe_count)}
    dupe_map: dict[bytes, tuple[str, int]] = {}

    total_lines = 0
    parse_errors = 0
    written = 0

    t0 = time.monotonic()

    for lineno, raw in enumerate(in_stream, 1):
        line = raw.rstrip("\n")
        if not line:
            continue
        total_lines += 1

        # -- Parse ------------------------------------------------------------
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            print(f"[warn] line {lineno}: JSON parse error — {exc}", file=sys.stderr)
            parse_errors += 1
            continue

        if not isinstance(record, dict):
            print(
                f"[warn] line {lineno}: expected JSON object, got "
                f"{type(record).__name__} — skipping",
                file=sys.stderr,
            )
            parse_errors += 1
            continue

        # -- Key / digest -----------------------------------------------------
        kb = key_bytes(record, fields)
        d = digest8(kb)

        # -- Duplicate check (memory first, then disk) -------------------------
        if d in mem_set or (disk is not None and d in disk):
            if d not in dupe_map:
                # Store a truncated repr for the stats summary.
                repr_str = kb.decode(errors="replace")
                if len(repr_str) > _KEY_REPR_MAX:
                    repr_str = repr_str[:_KEY_REPR_MAX - 1] + "…"
                dupe_map[d] = (repr_str, 0)
            key_repr, cnt = dupe_map[d]
            dupe_map[d] = (key_repr, cnt + 1)
            continue

        # -- New record -------------------------------------------------------
        mem_set.add(d)
        out_stream.write(line + "\n")
        written += 1

        # -- Spill check ------------------------------------------------------
        if len(mem_set) >= spill_at:
            if disk is None:
                fd, tmp_path = tempfile.mkstemp(suffix=".db", prefix="dedupe_spill_")
                os.close(fd)
                disk = DiskSet(tmp_path)
            disk.bulk_add(mem_set)
            mem_set.clear()
            spill_count += 1
            print(
                f"[info] spill #{spill_count}: flushed {spill_at:,} digests to disk",
                file=sys.stderr,
            )

    elapsed = time.monotonic() - t0
    total_dupes = sum(cnt for _, cnt in dupe_map.values())

    # -- Stats ----------------------------------------------------------------
    sep = "─" * 52
    print(f"\n{sep}", file=sys.stderr)
    print(f"  {'Lines read':<22}: {total_lines:>12,}", file=sys.stderr)
    print(f"  {'Written (unique)':<22}: {written:>12,}", file=sys.stderr)
    print(f"  {'Duplicates dropped':<22}: {total_dupes:>12,}", file=sys.stderr)
    print(f"  {'Parse errors skipped':<22}: {parse_errors:>12,}", file=sys.stderr)
    print(f"  {'Disk spills':<22}: {spill_count:>12,}", file=sys.stderr)
    print(
        f"  {'In-memory digests':<22}: {len(mem_set):>12,}  "
        f"(~{len(mem_set) * 40 // 1024} KiB est.)",
        file=sys.stderr,
    )
    print(f"  {'Elapsed':<22}: {elapsed:>11.3f}s", file=sys.stderr)
    if total_lines:
        rate = total_lines / elapsed if elapsed > 0 else float("inf")
        print(f"  {'Throughput':<22}: {rate:>10,.0f} lines/s", file=sys.stderr)
    print(sep, file=sys.stderr)

    if dupe_map and top_dupes > 0:
        ranked = sorted(dupe_map.items(), key=lambda kv: kv[1][1], reverse=True)
        shown = ranked[:top_dupes]
        print(
            f"\n  Top {len(shown)} duplicate key"
            f"{'s' if len(shown) != 1 else ''} by frequency:",
            file=sys.stderr,
        )
        for _d, (key_repr, cnt) in shown:
            print(f"    {cnt:>10,}x  {key_repr}", file=sys.stderr)
        if len(ranked) > top_dupes:
            print(
                f"    … and {len(ranked) - top_dupes:,} more duplicate key(s)",
                file=sys.stderr,
            )
        print(file=sys.stderr)

    # -- Cleanup --------------------------------------------------------------
    if disk is not None:
        disk.cleanup()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="dedupe.py",
        description=(
            "Streaming JSON Lines deduplicator.  "
            "Deduplicates by composite key using 8-byte BLAKE2b digests "
            "with configurable on-disk spill."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples\n"
            "--------\n"
            "  # Deduplicate by all fields, writing clean output to out.jsonl\n"
            "  python dedupe.py records.jsonl --output out.jsonl\n\n"
            "  # Composite key on 'user_id' and 'event_type' fields\n"
            "  python dedupe.py records.jsonl --key user_id event_type\n\n"
            "  # Tight memory budget: spill every 100k entries\n"
            "  python dedupe.py --spill-at 100000 < records.jsonl > clean.jsonl\n"
        ),
    )
    ap.add_argument(
        "input",
        nargs="?",
        metavar="INPUT",
        help="Input JSON Lines file (default: stdin; use - for explicit stdin)",
    )
    ap.add_argument(
        "--key",
        nargs="+",
        metavar="FIELD",
        help=(
            "JSON fields that form the composite dedup key.  "
            "Default: all fields in the object (sorted)."
        ),
    )
    ap.add_argument(
        "--spill-at",
        type=int,
        default=1_000_000,
        metavar="N",
        help=(
            "Flush in-memory digest set to disk when it reaches N entries.  "
            "Each entry costs ~40 bytes; 1 M ≈ 40 MB.  Default: 1000000."
        ),
    )
    ap.add_argument(
        "--output",
        metavar="FILE",
        help="Destination for deduplicated JSON Lines (default: stdout)",
    )
    ap.add_argument(
        "--top-dupes",
        type=int,
        default=20,
        metavar="N",
        help="Number of highest-frequency duplicate keys to display.  Default: 20.",
    )
    return ap


def main() -> None:
    ap = build_parser()
    args = ap.parse_args()

    if args.spill_at < 1:
        ap.error("--spill-at must be a positive integer")
    if args.top_dupes < 0:
        ap.error("--top-dupes must be >= 0")

    # Input stream
    if args.input and args.input != "-":
        try:
            in_stream = open(args.input, encoding="utf-8")
        except OSError as exc:
            print(f"error: cannot open input '{args.input}': {exc}", file=sys.stderr)
            sys.exit(1)
    else:
        in_stream = sys.stdin

    # Output stream
    if args.output:
        try:
            out_stream = open(args.output, "w", encoding="utf-8")
        except OSError as exc:
            print(f"error: cannot open output '{args.output}': {exc}", file=sys.stderr)
            if in_stream is not sys.stdin:
                in_stream.close()
            sys.exit(1)
    else:
        out_stream = sys.stdout

    try:
        run(
            in_stream=in_stream,
            out_stream=out_stream,
            fields=args.key,
            spill_at=args.spill_at,
            top_dupes=args.top_dupes,
        )
    except BrokenPipeError:
        # Suppress the traceback when stdout is piped and the consumer exits.
        sys.stderr.close()
    except KeyboardInterrupt:
        print("\n[interrupted]", file=sys.stderr)
        sys.exit(1)
    finally:
        if in_stream is not sys.stdin:
            in_stream.close()
        if out_stream is not sys.stdout:
            out_stream.close()


if __name__ == "__main__":
    main()
