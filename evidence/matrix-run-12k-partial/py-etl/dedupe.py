#!/usr/bin/env python3
"""
dedupe.py — streaming JSON Lines deduplicator with bounded memory

Reads JSON Lines from a file or stdin, deduplicates records by a composite
key, and writes clean (first-seen) records to a file or stdout.  Stats are
printed to stderr.

Memory strategy
---------------
An in-memory set holds 8-byte blake2b digests of composite key values.
When the set reaches --spill-at items the entire set is flushed to a
temporary SQLite database and cleared; subsequent membership checks search
both the in-memory set and the on-disk table.  Each digest is 8 bytes of
raw BLOB; SQLite stores them compactly and uses a B-tree index on the
PRIMARY KEY for O(log n) lookups.

Usage examples
--------------
    python dedupe.py --key id --input records.jsonl --output clean.jsonl
    python dedupe.py --key user_id event_type -i in.jsonl -o out.jsonl -n 500000
    cat records.jsonl | python dedupe.py --key id name > clean.jsonl
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
import time
from typing import IO, Iterator


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DIGEST_BYTES = 8          # blake2b digest size in bytes  →  2^64 key-space
DEFAULT_SPILL_AT = 1_000_000
DEFAULT_TOP_DUPES = 10


# ---------------------------------------------------------------------------
# Digest helpers
# ---------------------------------------------------------------------------

def _make_digest(key_values: tuple) -> bytes:
    """Return an 8-byte blake2b digest of the composite key tuple.

    Key values are serialised to canonical JSON (deterministic, handles None,
    int, str, bool, float) before hashing so that equivalent values always
    produce the same digest regardless of Python object identity.
    """
    canonical = json.dumps(key_values, separators=(",", ":"), ensure_ascii=False)
    return hashlib.blake2b(canonical.encode(), digest_size=DIGEST_BYTES).digest()


def _extract_key(record: dict, fields: list[str]) -> tuple:
    """Pull *fields* from *record* in order; missing fields become None."""
    return tuple(record.get(f) for f in fields)


# ---------------------------------------------------------------------------
# Bounded deduplicator
# ---------------------------------------------------------------------------

class BoundedDeduplicator:
    """In-memory digest set with automatic spill to an on-disk SQLite table.

    Parameters
    ----------
    spill_at:
        Maximum number of digests to hold in memory before spilling to disk.
    tmp_dir:
        Directory for the temporary database file (default: system temp dir).
    """

    def __init__(self, spill_at: int, tmp_dir: str | None = None) -> None:
        if spill_at < 1:
            raise ValueError("spill_at must be >= 1")
        self._spill_at = spill_at
        self._mem: set[bytes] = set()
        self._db: sqlite3.Connection | None = None
        self._db_path: str | None = None
        self._tmp_dir = tmp_dir
        self._spill_count = 0

    # ------------------------------------------------------------------
    # Private helpers
    # ------------------------------------------------------------------

    def _ensure_db(self) -> sqlite3.Connection:
        if self._db is not None:
            return self._db
        fd, path = tempfile.mkstemp(
            suffix=".db", prefix="dedupe_", dir=self._tmp_dir
        )
        os.close(fd)
        self._db_path = path
        conn = sqlite3.connect(path, check_same_thread=True)
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous  = NORMAL")
        conn.execute("PRAGMA cache_size   = -16384")   # 16 MiB page cache
        conn.execute(
            "CREATE TABLE IF NOT EXISTS seen (d BLOB NOT NULL PRIMARY KEY)"
        )
        conn.commit()
        self._db = conn
        return conn

    def _spill(self) -> None:
        """Flush the in-memory set to disk and clear it."""
        db = self._ensure_db()
        db.executemany(
            "INSERT OR IGNORE INTO seen VALUES (?)",
            ((d,) for d in self._mem),
        )
        db.commit()
        self._mem.clear()
        self._spill_count += 1

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    def __contains__(self, digest: bytes) -> bool:
        if digest in self._mem:
            return True
        if self._db is not None:
            row = self._db.execute(
                "SELECT 1 FROM seen WHERE d = ? LIMIT 1", (digest,)
            ).fetchone()
            return row is not None
        return False

    def add(self, digest: bytes) -> None:
        """Mark *digest* as seen; spill to disk if the memory limit is reached."""
        if len(self._mem) >= self._spill_at:
            self._spill()
        self._mem.add(digest)

    def close(self) -> None:
        """Release the database connection and delete the temporary file."""
        if self._db is not None:
            try:
                self._db.close()
            except Exception:
                pass
            self._db = None
        if self._db_path:
            for suffix in ("", "-wal", "-shm"):
                target = self._db_path + suffix
                try:
                    os.unlink(target)
                except FileNotFoundError:
                    pass
            self._db_path = None

    # ------------------------------------------------------------------
    # Diagnostic properties
    # ------------------------------------------------------------------

    @property
    def spill_count(self) -> int:
        """Number of times the in-memory set has been flushed to disk."""
        return self._spill_count

    @property
    def mem_size(self) -> int:
        """Current number of digests held in memory."""
        return len(self._mem)

    def disk_size(self) -> int:
        """Number of digests persisted to disk (0 if no spill has occurred)."""
        if self._db is None:
            return 0
        row = self._db.execute("SELECT COUNT(*) FROM seen").fetchone()
        return int(row[0]) if row else 0


# ---------------------------------------------------------------------------
# Core ETL
# ---------------------------------------------------------------------------

def _iter_lines(stream: IO[str]) -> Iterator[tuple[int, str]]:
    """Yield (1-based line number, stripped line) skipping blank lines."""
    for lineno, raw in enumerate(stream, 1):
        line = raw.rstrip("\n")
        if line:
            yield lineno, line


def process(
    input_stream: IO[str],
    output_stream: IO[str],
    key_fields: list[str],
    spill_at: int,
    top_dupes: int,
) -> dict:
    """Stream-deduplicate *input_stream* into *output_stream*.

    Returns a statistics dictionary.
    """
    deduper = BoundedDeduplicator(spill_at=spill_at)

    # dupe_counts maps digest → [human-readable key repr, count].
    # Only digests that have been seen more than once are stored here.
    # In the common case the number of distinct duplicated keys is small;
    # for pathological inputs (many distinct keys each duplicated once) this
    # dict can grow, but it stores only duplicates — never all unique keys.
    dupe_counts: dict[bytes, list] = {}

    total = 0
    dupes = 0
    errors = 0
    t_start = time.monotonic()

    for lineno, raw in _iter_lines(input_stream):
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as exc:
            print(f"[warn] line {lineno}: {exc}", file=sys.stderr)
            errors += 1
            continue

        if not isinstance(record, dict):
            print(
                f"[warn] line {lineno}: expected JSON object, got "
                f"{type(record).__name__} — skipped",
                file=sys.stderr,
            )
            errors += 1
            continue

        total += 1
        key_vals = _extract_key(record, key_fields)
        digest = _make_digest(key_vals)

        if digest in deduper:
            dupes += 1
            entry = dupe_counts.get(digest)
            if entry is None:
                dupe_counts[digest] = [str(key_vals), 1]
            else:
                entry[1] += 1
        else:
            deduper.add(digest)
            output_stream.write(raw + "\n")

    elapsed = time.monotonic() - t_start

    # Build top-dupes list (sorted descending by count, truncated to top_dupes)
    top: list[dict] = []
    if dupe_counts:
        ranked = sorted(dupe_counts.values(), key=lambda x: x[1], reverse=True)
        top = [
            {"key": entry[0], "count": entry[1]}
            for entry in ranked[:top_dupes]
        ]

    stats: dict = {
        "total_records":    total,
        "unique_records":   total - dupes,
        "duplicate_records": dupes,
        "parse_errors":     errors,
        "elapsed_seconds":  round(elapsed, 3),
        "spill_count":      deduper.spill_count,
        "mem_digests":      deduper.mem_size,
        "disk_digests":     deduper.disk_size(),
        "top_duplicate_keys": top,
    }

    deduper.close()
    return stats


# ---------------------------------------------------------------------------
# Stats printer
# ---------------------------------------------------------------------------

def print_stats(stats: dict) -> None:
    total = stats["total_records"]
    dupes = stats["duplicate_records"]
    pct   = dupes / total * 100 if total else 0.0

    rows = [
        ("total records",  f"{total:>12,}"),
        ("unique records",  f"{stats['unique_records']:>12,}"),
        ("duplicates",      f"{dupes:>12,}  ({pct:.2f}%)"),
        ("parse errors",    f"{stats['parse_errors']:>12,}"),
        ("elapsed",         f"{stats['elapsed_seconds']:>11.3f}s"),
        ("spills to disk",  f"{stats['spill_count']:>12,}"),
        ("mem digests",     f"{stats['mem_digests']:>12,}"),
        ("disk digests",    f"{stats['disk_digests']:>12,}"),
    ]

    width = 48
    sep = "─" * width
    lines = [sep]
    for label, value in rows:
        lines.append(f"  {label:<18} {value}")

    top = stats.get("top_duplicate_keys")
    if top:
        lines.append(f"  {'top duplicate keys':<18}")
        for entry in top:
            lines.append(f"    {entry['count']:>8,}×  {entry['key']}")

    lines.append(sep)
    print("\n".join(lines), file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dedupe.py",
        description=(
            "Deduplicate JSON Lines by a composite key using a bounded-memory "
            "strategy (in-memory blake2b digest set with on-disk SQLite spill)."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--key", "-k",
        nargs="+",
        required=True,
        metavar="FIELD",
        help="one or more JSON field names forming the composite key",
    )
    parser.add_argument(
        "--input", "-i",
        metavar="FILE",
        default="-",
        help="input JSON Lines file (default: stdin)",
    )
    parser.add_argument(
        "--output", "-o",
        metavar="FILE",
        default="-",
        help="output JSON Lines file (default: stdout)",
    )
    parser.add_argument(
        "--spill-at", "-n",
        type=int,
        default=DEFAULT_SPILL_AT,
        metavar="N",
        dest="spill_at",
        help=(
            f"spill in-memory digest set to disk after N items "
            f"(default: {DEFAULT_SPILL_AT:,}); "
            f"each digest is {DIGEST_BYTES} bytes so N=1_000_000 ≈ 8 MB raw"
        ),
    )
    parser.add_argument(
        "--top-dupes",
        type=int,
        default=DEFAULT_TOP_DUPES,
        metavar="K",
        dest="top_dupes",
        help=f"print the top K most-duplicated keys in stats (default: {DEFAULT_TOP_DUPES})",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.spill_at < 1:
        parser.error("--spill-at must be >= 1")
    if args.top_dupes < 0:
        parser.error("--top-dupes must be >= 0")

    # Open input
    if args.input == "-":
        in_stream: IO[str] = sys.stdin
        close_in = False
    else:
        in_stream = open(args.input, "r", encoding="utf-8")
        close_in = True

    # Open output
    if args.output == "-":
        out_stream: IO[str] = sys.stdout
        close_out = False
    else:
        out_stream = open(args.output, "w", encoding="utf-8")
        close_out = True

    try:
        stats = process(
            input_stream=in_stream,
            output_stream=out_stream,
            key_fields=args.key,
            spill_at=args.spill_at,
            top_dupes=args.top_dupes,
        )
    finally:
        if close_in:
            in_stream.close()
        if close_out:
            out_stream.close()

    print_stats(stats)
    return 0


if __name__ == "__main__":
    sys.exit(main())
