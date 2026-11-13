#!/usr/bin/env python3
"""
dedupe.py — streaming JSON Lines deduplicator

Strategy
--------
Each record's composite key is hashed to an 8-byte blake2b digest and stored
in an in-memory set.  When the set reaches --max-memory items it is flushed to
a SQLite table (the "spill store") and the memory set is cleared.  Lookups
always check memory first, then the spill store, so the memory footprint stays
bounded at roughly max_memory × 8 bytes plus SQLite overhead.

Collision note: 8-byte (64-bit) digests give a ~0.5 % birthday-collision
probability at ~600 M unique keys.  Raise digest_size in make_digest() for
larger datasets.

Usage
-----
  python dedupe.py -k id < in.jsonl > out.jsonl
  python dedupe.py -k user_id event_type -n 500000 -i in.jsonl -o out.jsonl
  python dedupe.py -k id --db /tmp/seen.db -i in.jsonl -o out.jsonl
"""

import argparse
import hashlib
import json
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Hashing
# ---------------------------------------------------------------------------

def make_digest(record: dict, key_fields: list[str]) -> bytes:
    """Return an 8-byte blake2b digest of the composite key extracted from *record*."""
    parts = (
        json.dumps(record.get(f), sort_keys=True, separators=(",", ":"))
        for f in key_fields
    )
    raw = "\x00".join(parts).encode("utf-8")
    return hashlib.blake2b(raw, digest_size=8).digest()


def key_label(record: dict, key_fields: list[str]) -> str:
    """Human-readable composite key string, used only for the stats report."""
    return ", ".join(
        f"{f}={json.dumps(record.get(f), separators=(',', ':'))}"
        for f in key_fields
    )


# ---------------------------------------------------------------------------
# On-disk spill store (SQLite)
# ---------------------------------------------------------------------------

class SpillStore:
    """SQLite-backed set of 8-byte BLOB digests."""

    def __init__(self, path: str) -> None:
        self.conn = sqlite3.connect(path, check_same_thread=True)
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.execute("PRAGMA cache_size=-8192")          # 8 MB page cache
        self.conn.execute(
            "CREATE TABLE IF NOT EXISTS seen (d BLOB NOT NULL PRIMARY KEY WITHOUT ROWID)"
        )
        self.conn.commit()

    def contains(self, digest: bytes) -> bool:
        return self.conn.execute(
            "SELECT 1 FROM seen WHERE d = ?", (digest,)
        ).fetchone() is not None

    def add_batch(self, digests: set[bytes]) -> None:
        self.conn.executemany(
            "INSERT OR IGNORE INTO seen VALUES (?)", ((d,) for d in digests)
        )
        self.conn.commit()

    def close(self) -> None:
        self.conn.close()


# ---------------------------------------------------------------------------
# Bounded deduplicator
# ---------------------------------------------------------------------------

class BoundedDeduplicator:
    """
    Tracks seen digests with an in-memory set capped at *max_memory_items*.
    Excess digests spill to *SpillStore*.
    """

    def __init__(
        self,
        key_fields: list[str],
        max_memory_items: int,
        db_path: str,
    ) -> None:
        self.key_fields = key_fields
        self.max_memory_items = max_memory_items
        self._mem: set[bytes] = set()
        self._disk = SpillStore(db_path)
        self.spill_count = 0

    # Returns (is_duplicate, digest)
    def check_and_record(self, record: dict) -> tuple[bool, bytes]:
        digest = make_digest(record, self.key_fields)

        if digest in self._mem:
            return True, digest

        if self._disk.contains(digest):
            return True, digest

        # First time we see this digest.
        self._mem.add(digest)
        if len(self._mem) >= self.max_memory_items:
            self._spill()

        return False, digest

    def _spill(self) -> None:
        self._disk.add_batch(self._mem)
        self._mem.clear()
        self.spill_count += 1

    def flush(self) -> None:
        """Flush the remaining in-memory set to disk."""
        if self._mem:
            self._spill()

    def close(self) -> None:
        self._disk.close()


# ---------------------------------------------------------------------------
# Core ETL loop
# ---------------------------------------------------------------------------

def process(
    in_path: str,
    out_path: str,
    key_fields: list[str],
    max_memory_items: int,
    db_path: str,
) -> dict:
    deduper = BoundedDeduplicator(key_fields, max_memory_items, db_path)

    total = unique = dupes = errors = 0
    dupe_counts: dict[str, int] = {}   # key label → extra-occurrence count
    t0 = time.monotonic()

    in_fh  = sys.stdin  if in_path  == "-" else open(in_path,  "r", encoding="utf-8")
    out_fh = sys.stdout if out_path == "-" else open(out_path, "w", encoding="utf-8")

    try:
        for lineno, raw in enumerate(in_fh, 1):
            line = raw.strip()
            if not line:
                continue

            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                errors += 1
                print(f"dedupe: line {lineno}: {exc}", file=sys.stderr)
                continue

            if not isinstance(record, dict):
                errors += 1
                print(
                    f"dedupe: line {lineno}: expected JSON object, got {type(record).__name__}",
                    file=sys.stderr,
                )
                continue

            total += 1
            is_dupe, _ = deduper.check_and_record(record)

            if is_dupe:
                dupes += 1
                label = key_label(record, key_fields)
                dupe_counts[label] = dupe_counts.get(label, 0) + 1
            else:
                unique += 1
                out_fh.write(line + "\n")

    finally:
        if in_path  != "-":
            in_fh.close()
        if out_path != "-":
            out_fh.close()
        deduper.flush()
        deduper.close()

    return {
        "total":        total,
        "unique":       unique,
        "duplicates":   dupes,
        "parse_errors": errors,
        "spill_count":  deduper.spill_count,
        "elapsed":      time.monotonic() - t0,
        "dupe_counts":  dupe_counts,
    }


# ---------------------------------------------------------------------------
# Stats reporter
# ---------------------------------------------------------------------------

def print_stats(stats: dict, key_fields: list[str], top_n: int = 25) -> None:
    sep = "─" * 52
    s = sys.stderr
    print(sep, file=s)
    print(f"  Key fields      : {', '.join(key_fields)}", file=s)
    print(f"  Total records   : {stats['total']:>12,}", file=s)
    print(f"  Unique records  : {stats['unique']:>12,}", file=s)
    print(f"  Duplicates      : {stats['duplicates']:>12,}", file=s)
    print(f"  Parse errors    : {stats['parse_errors']:>12,}", file=s)
    print(f"  Disk spills     : {stats['spill_count']:>12,}", file=s)
    print(f"  Elapsed         : {stats['elapsed']:>12.3f}s", file=s)

    dc = stats["dupe_counts"]
    if dc:
        ranked = sorted(dc.items(), key=lambda kv: kv[1], reverse=True)
        print("", file=s)
        print(f"  Duplicate counts by key (top {top_n}):", file=s)
        for label, count in ranked[:top_n]:
            print(f"    {count:>8,}  {label}", file=s)
        if len(ranked) > top_n:
            print(f"    ... and {len(ranked) - top_n:,} more keys with duplicates", file=s)

    print(sep, file=s)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="dedupe.py",
        description="Stream-deduplicate JSON Lines by a composite key.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument(
        "-k", "--key",
        nargs="+",
        required=True,
        metavar="FIELD",
        help="One or more JSON field names that form the composite dedup key.",
    )
    p.add_argument(
        "-n", "--max-memory",
        type=int,
        default=1_000_000,
        metavar="N",
        help=(
            "Maximum digest count held in memory before spilling to disk "
            "(default: 1_000_000 ≈ 8 MB of digests)."
        ),
    )
    p.add_argument(
        "-i", "--input",
        default="-",
        metavar="FILE",
        help="Input JSON Lines file (default: stdin).",
    )
    p.add_argument(
        "-o", "--output",
        default="-",
        metavar="FILE",
        help="Output JSON Lines file (default: stdout).",
    )
    p.add_argument(
        "--db",
        default=None,
        metavar="PATH",
        help=(
            "Path for the SQLite spill store.  "
            "Omit to use a temp file that is deleted on exit."
        ),
    )
    p.add_argument(
        "--top",
        type=int,
        default=25,
        metavar="N",
        help="Number of highest-duplicate keys to show in the stats report (default: 25).",
    )
    return p


def main() -> None:
    args = build_parser().parse_args()

    if args.max_memory < 1:
        sys.exit("dedupe: --max-memory must be at least 1")

    # Resolve the spill-store path, creating a temp file if needed.
    _tmp_db_path: str | None = None
    if args.db:
        db_path = args.db
    else:
        with tempfile.NamedTemporaryFile(suffix=".dedupe.db", delete=False) as fh:
            _tmp_db_path = fh.name
        db_path = _tmp_db_path

    try:
        stats = process(
            in_path=args.input,
            out_path=args.output,
            key_fields=args.key,
            max_memory_items=args.max_memory,
            db_path=db_path,
        )
        print_stats(stats, args.key, top_n=args.top)
    finally:
        if _tmp_db_path is not None:
            for suffix in ("", "-wal", "-shm"):
                Path(_tmp_db_path + suffix).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
